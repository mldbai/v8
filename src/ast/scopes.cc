// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ast/scopes.h"

#include <set>

#include "src/accessors.h"
#include "src/bootstrapper.h"
#include "src/messages.h"
#include "src/parsing/parse-info.h"

namespace v8 {
namespace internal {

// ----------------------------------------------------------------------------
// Implementation of LocalsMap
//
// Note: We are storing the handle locations as key values in the hash map.
//       When inserting a new variable via Declare(), we rely on the fact that
//       the handle location remains alive for the duration of that variable
//       use. Because a Variable holding a handle with the same location exists
//       this is ensured.

VariableMap::VariableMap(Zone* zone)
    : ZoneHashMap(ZoneHashMap::PointersMatch, 8, ZoneAllocationPolicy(zone)) {}

Variable* VariableMap::Declare(Zone* zone, Scope* scope,
                               const AstRawString* name, VariableMode mode,
                               Variable::Kind kind,
                               InitializationFlag initialization_flag,
                               MaybeAssignedFlag maybe_assigned_flag,
                               bool* added) {
  // AstRawStrings are unambiguous, i.e., the same string is always represented
  // by the same AstRawString*.
  // FIXME(marja): fix the type of Lookup.
  Entry* p =
      ZoneHashMap::LookupOrInsert(const_cast<AstRawString*>(name), name->hash(),
                                  ZoneAllocationPolicy(zone));
  if (added) *added = p->value == nullptr;
  if (p->value == nullptr) {
    // The variable has not been declared yet -> insert it.
    DCHECK_EQ(name, p->key);
    p->value = new (zone) Variable(scope, name, mode, kind, initialization_flag,
                                   maybe_assigned_flag);
  }
  return reinterpret_cast<Variable*>(p->value);
}

void VariableMap::Remove(Variable* var) {
  const AstRawString* name = var->raw_name();
  ZoneHashMap::Remove(const_cast<AstRawString*>(name), name->hash());
}

void VariableMap::Add(Zone* zone, Variable* var) {
  const AstRawString* name = var->raw_name();
  Entry* p =
      ZoneHashMap::LookupOrInsert(const_cast<AstRawString*>(name), name->hash(),
                                  ZoneAllocationPolicy(zone));
  DCHECK_NULL(p->value);
  DCHECK_EQ(name, p->key);
  p->value = var;
}

Variable* VariableMap::Lookup(const AstRawString* name) {
  Entry* p = ZoneHashMap::Lookup(const_cast<AstRawString*>(name), name->hash());
  if (p != NULL) {
    DCHECK(reinterpret_cast<const AstRawString*>(p->key) == name);
    DCHECK(p->value != NULL);
    return reinterpret_cast<Variable*>(p->value);
  }
  return NULL;
}

SloppyBlockFunctionMap::SloppyBlockFunctionMap(Zone* zone)
    : ZoneHashMap(ZoneHashMap::PointersMatch, 8, ZoneAllocationPolicy(zone)) {}

void SloppyBlockFunctionMap::Declare(Zone* zone, const AstRawString* name,
                                     SloppyBlockFunctionStatement* stmt) {
  // AstRawStrings are unambiguous, i.e., the same string is always represented
  // by the same AstRawString*.
  Entry* p =
      ZoneHashMap::LookupOrInsert(const_cast<AstRawString*>(name), name->hash(),
                                  ZoneAllocationPolicy(zone));
  stmt->set_next(static_cast<SloppyBlockFunctionStatement*>(p->value));
  p->value = stmt;
}


// ----------------------------------------------------------------------------
// Implementation of Scope

Scope::Scope(Zone* zone, ScopeType scope_type)
    : zone_(zone),
      outer_scope_(nullptr),
      variables_(zone),
      locals_(4, zone),
      decls_(4, zone),
      scope_type_(scope_type) {
  DCHECK(scope_type == SCRIPT_SCOPE || scope_type == WITH_SCOPE);
  SetDefaults();
#ifdef DEBUG
  if (scope_type == WITH_SCOPE) {
    already_resolved_ = true;
  }
#endif
}

Scope::Scope(Zone* zone, Scope* outer_scope, ScopeType scope_type)
    : zone_(zone),
      outer_scope_(outer_scope),
      variables_(zone),
      locals_(4, zone),
      decls_(4, zone),
      scope_type_(scope_type) {
  DCHECK_NE(SCRIPT_SCOPE, scope_type);
  SetDefaults();
  set_language_mode(outer_scope->language_mode());
  force_context_allocation_ =
      !is_function_scope() && outer_scope->has_forced_context_allocation();
  outer_scope_->AddInnerScope(this);
}

Scope::Snapshot::Snapshot(Scope* scope)
    : outer_scope_(scope),
      top_inner_scope_(scope->inner_scope_),
      top_unresolved_(scope->unresolved_),
      top_local_(scope->GetClosureScope()->locals_.length()),
      top_decl_(scope->GetClosureScope()->decls_.length()) {}

DeclarationScope::DeclarationScope(Zone* zone)
    : Scope(zone),
      function_kind_(kNormalFunction),
      params_(4, zone),
      sloppy_block_function_map_(zone) {
  SetDefaults();
}

DeclarationScope::DeclarationScope(Zone* zone, Scope* outer_scope,
                                   ScopeType scope_type,
                                   FunctionKind function_kind)
    : Scope(zone, outer_scope, scope_type),
      function_kind_(function_kind),
      params_(4, zone),
      sloppy_block_function_map_(zone) {
  SetDefaults();
  asm_function_ = outer_scope_->IsAsmModule();
}

ModuleScope::ModuleScope(DeclarationScope* script_scope,
                         AstValueFactory* ast_value_factory)
    : DeclarationScope(ast_value_factory->zone(), script_scope, MODULE_SCOPE) {
  Zone* zone = ast_value_factory->zone();
  module_descriptor_ = new (zone) ModuleDescriptor(zone);
  set_language_mode(STRICT);
  DeclareThis(ast_value_factory);
}

Scope::Scope(Zone* zone, ScopeType scope_type, Handle<ScopeInfo> scope_info)
    : zone_(zone),
      outer_scope_(nullptr),
      variables_(zone),
      locals_(0, zone),
      decls_(0, zone),
      scope_info_(scope_info),
      scope_type_(scope_type) {
  DCHECK(!scope_info.is_null());
  SetDefaults();
#ifdef DEBUG
  already_resolved_ = true;
#endif
  if (scope_info->CallsEval()) RecordEvalCall();
  set_language_mode(scope_info->language_mode());
  num_heap_slots_ = scope_info->ContextLength();
  DCHECK_LE(Context::MIN_CONTEXT_SLOTS, num_heap_slots_);
}

DeclarationScope::DeclarationScope(Zone* zone, ScopeType scope_type,
                                   Handle<ScopeInfo> scope_info)
    : Scope(zone, scope_type, scope_info),
      function_kind_(scope_info->function_kind()),
      params_(0, zone),
      sloppy_block_function_map_(zone) {
  SetDefaults();
}

Scope::Scope(Zone* zone, const AstRawString* catch_variable_name)
    : zone_(zone),
      outer_scope_(nullptr),
      variables_(zone),
      locals_(0, zone),
      decls_(0, zone),
      scope_type_(CATCH_SCOPE) {
  SetDefaults();
#ifdef DEBUG
  already_resolved_ = true;
#endif
  Variable* variable = Declare(zone, this, catch_variable_name, VAR,
                               Variable::NORMAL, kCreatedInitialized);
  AllocateHeapSlot(variable);
}

void DeclarationScope::SetDefaults() {
  is_declaration_scope_ = true;
  has_simple_parameters_ = true;
  asm_module_ = false;
  asm_function_ = false;
  force_eager_compilation_ = false;
  has_arguments_parameter_ = false;
  scope_uses_super_property_ = false;
  has_rest_ = false;
  receiver_ = nullptr;
  new_target_ = nullptr;
  function_ = nullptr;
  arguments_ = nullptr;
  this_function_ = nullptr;
  arity_ = 0;
}

void Scope::SetDefaults() {
#ifdef DEBUG
  scope_name_ = nullptr;
  already_resolved_ = false;
#endif
  inner_scope_ = nullptr;
  sibling_ = nullptr;
  unresolved_ = nullptr;

  start_position_ = kNoSourcePosition;
  end_position_ = kNoSourcePosition;

  num_stack_slots_ = 0;
  num_heap_slots_ = Context::MIN_CONTEXT_SLOTS;

  set_language_mode(SLOPPY);

  scope_calls_eval_ = false;
  scope_nonlinear_ = false;
  is_hidden_ = false;
  is_debug_evaluate_scope_ = false;

  inner_scope_calls_eval_ = false;
  force_context_allocation_ = false;

  is_declaration_scope_ = false;
}

bool Scope::HasSimpleParameters() {
  DeclarationScope* scope = GetClosureScope();
  return !scope->is_function_scope() || scope->has_simple_parameters();
}

bool Scope::IsAsmModule() const {
  return is_function_scope() && AsDeclarationScope()->asm_module();
}

bool Scope::IsAsmFunction() const {
  return is_function_scope() && AsDeclarationScope()->asm_function();
}

Scope* Scope::DeserializeScopeChain(Isolate* isolate, Zone* zone,
                                    Context* context,
                                    DeclarationScope* script_scope,
                                    AstValueFactory* ast_value_factory,
                                    DeserializationMode deserialization_mode) {
  // Reconstruct the outer scope chain from a closure's context chain.
  Scope* current_scope = nullptr;
  Scope* innermost_scope = nullptr;
  Scope* outer_scope = nullptr;
  while (!context->IsNativeContext()) {
    if (context->IsWithContext() || context->IsDebugEvaluateContext()) {
      // For scope analysis, debug-evaluate is equivalent to a with scope.
      outer_scope = new (zone) Scope(zone, WITH_SCOPE);

      // TODO(yangguo): Remove once debug-evaluate properly keeps track of the
      // function scope in which we are evaluating.
      if (context->IsDebugEvaluateContext()) {
        outer_scope->set_is_debug_evaluate_scope();
      }
    } else if (context->IsScriptContext()) {
      // If we reach a script context, it's the outermost context with scope
      // info. The next context will be the native context. Install the scope
      // info of this script context onto the existing script scope to avoid
      // nesting script scopes.
      Handle<ScopeInfo> scope_info(context->scope_info(), isolate);
      script_scope->SetScriptScopeInfo(scope_info);
      DCHECK(context->previous()->IsNativeContext());
      break;
    } else if (context->IsFunctionContext()) {
      Handle<ScopeInfo> scope_info(context->closure()->shared()->scope_info(),
                                   isolate);
      // TODO(neis): For an eval scope, we currently create an ordinary function
      // context.  This is wrong and needs to be fixed.
      // https://bugs.chromium.org/p/v8/issues/detail?id=5295
      DCHECK(scope_info->scope_type() == FUNCTION_SCOPE ||
             scope_info->scope_type() == EVAL_SCOPE);
      outer_scope =
          new (zone) DeclarationScope(zone, FUNCTION_SCOPE, scope_info);
      if (scope_info->IsAsmFunction())
        outer_scope->AsDeclarationScope()->set_asm_function();
      if (scope_info->IsAsmModule())
        outer_scope->AsDeclarationScope()->set_asm_module();
    } else if (context->IsBlockContext()) {
      Handle<ScopeInfo> scope_info(context->scope_info(), isolate);
      DCHECK_EQ(scope_info->scope_type(), BLOCK_SCOPE);
      if (scope_info->is_declaration_scope()) {
        outer_scope =
            new (zone) DeclarationScope(zone, BLOCK_SCOPE, scope_info);
      } else {
        outer_scope = new (zone) Scope(zone, BLOCK_SCOPE, scope_info);
      }
    } else {
      DCHECK(context->IsCatchContext());
      String* name = context->catch_name();
      outer_scope = new (zone)
          Scope(zone, ast_value_factory->GetString(handle(name, isolate)));
    }
    if (current_scope != nullptr) {
      outer_scope->AddInnerScope(current_scope);
    }
    current_scope = outer_scope;
    if (deserialization_mode == DeserializationMode::kDeserializeOffHeap) {
      current_scope->DeserializeScopeInfo(isolate, ast_value_factory);
    }
    if (innermost_scope == nullptr) innermost_scope = current_scope;
    context = context->previous();
  }

  if (innermost_scope == nullptr) return script_scope;
  script_scope->AddInnerScope(current_scope);
  script_scope->PropagateScopeInfo();
  return innermost_scope;
}

void Scope::DeserializeScopeInfo(Isolate* isolate,
                                 AstValueFactory* ast_value_factory) {
  if (scope_info_.is_null()) return;

  DCHECK(ThreadId::Current().Equals(isolate->thread_id()));

  // Internalize context local variables.
  for (int var = 0; var < scope_info_->ContextLocalCount(); ++var) {
    Handle<String> name_handle(scope_info_->ContextLocalName(var), isolate);
    const AstRawString* name = ast_value_factory->GetString(name_handle);
    int index = Context::MIN_CONTEXT_SLOTS + var;
    VariableMode mode = scope_info_->ContextLocalMode(var);
    InitializationFlag init_flag = scope_info_->ContextLocalInitFlag(var);
    MaybeAssignedFlag maybe_assigned_flag =
        scope_info_->ContextLocalMaybeAssignedFlag(var);
    VariableLocation location = VariableLocation::CONTEXT;
    Variable::Kind kind = Variable::NORMAL;
    if (index == scope_info_->ReceiverContextSlotIndex()) {
      kind = Variable::THIS;
    }

    Variable* result = variables_.Declare(zone(), this, name, mode, kind,
                                          init_flag, maybe_assigned_flag);
    result->AllocateTo(location, index);
  }

  // Internalize function proxy for this scope.
  if (scope_info_->HasFunctionName()) {
    Handle<String> name_handle(scope_info_->FunctionName(), isolate);
    const AstRawString* name = ast_value_factory->GetString(name_handle);
    VariableMode mode;
    int index = scope_info_->FunctionContextSlotIndex(*name_handle, &mode);
    if (index >= 0) {
      Variable* result = AsDeclarationScope()->DeclareFunctionVar(name);
      DCHECK_EQ(mode, result->mode());
      result->AllocateTo(VariableLocation::CONTEXT, index);
    }
  }

  scope_info_ = Handle<ScopeInfo>::null();
}

DeclarationScope* Scope::AsDeclarationScope() {
  DCHECK(is_declaration_scope());
  return static_cast<DeclarationScope*>(this);
}

const DeclarationScope* Scope::AsDeclarationScope() const {
  DCHECK(is_declaration_scope());
  return static_cast<const DeclarationScope*>(this);
}

ModuleScope* Scope::AsModuleScope() {
  DCHECK(is_module_scope());
  return static_cast<ModuleScope*>(this);
}

const ModuleScope* Scope::AsModuleScope() const {
  DCHECK(is_module_scope());
  return static_cast<const ModuleScope*>(this);
}

int Scope::num_parameters() const {
  return is_declaration_scope() ? AsDeclarationScope()->num_parameters() : 0;
}

void DeclarationScope::Analyze(ParseInfo* info) {
  DCHECK(info->literal() != NULL);
  DeclarationScope* scope = info->literal()->scope();

  // We are compiling one of three cases:
  // 1) top-level code,
  // 2) a function/eval/module on the top-level
  // 3) a function/eval in a scope that was already resolved.
  DCHECK(scope->scope_type() == SCRIPT_SCOPE ||
         scope->outer_scope()->scope_type() == SCRIPT_SCOPE ||
         scope->outer_scope()->already_resolved_);

  scope->AllocateVariables(info, false /* for_debugger */);

#ifdef DEBUG
  if (info->script_is_native() ? FLAG_print_builtin_scopes
                               : FLAG_print_scopes) {
    scope->Print();
  }
  scope->CheckScopePositions();
  scope->CheckZones();
#endif
}

void DeclarationScope::AnalyzeForDebugger(ParseInfo* info) {
  DCHECK(info->literal() != NULL);
  DeclarationScope* scope = info->literal()->scope();

  // We are compiling one of three cases:
  // 1) top-level code,
  // 2) a function/eval/module on the top-level
  // 3) a function/eval in a scope that was already resolved.
  DCHECK(scope->scope_type() == SCRIPT_SCOPE ||
         scope->outer_scope()->scope_type() == SCRIPT_SCOPE ||
         scope->outer_scope()->already_resolved_);

  scope->AllocateVariables(info, true /* for_debugger */);
}

void DeclarationScope::DeclareThis(AstValueFactory* ast_value_factory) {
  DCHECK(!already_resolved_);
  DCHECK(is_declaration_scope());
  DCHECK(has_this_declaration());

  bool subclass_constructor = IsSubclassConstructor(function_kind_);
  Variable* var = Declare(
      zone(), this, ast_value_factory->this_string(),
      subclass_constructor ? CONST : VAR, Variable::THIS,
      subclass_constructor ? kNeedsInitialization : kCreatedInitialized);
  receiver_ = var;
}

void DeclarationScope::DeclareDefaultFunctionVariables(
    AstValueFactory* ast_value_factory) {
  DCHECK(is_function_scope());
  DCHECK(!is_arrow_scope());
  // Declare 'arguments' variable which exists in all non arrow functions.
  // Note that it might never be accessed, in which case it won't be
  // allocated during variable allocation.
  arguments_ = Declare(zone(), this, ast_value_factory->arguments_string(), VAR,
                       Variable::ARGUMENTS, kCreatedInitialized);

  new_target_ = Declare(zone(), this, ast_value_factory->new_target_string(),
                        CONST, Variable::NORMAL, kCreatedInitialized);

  if (IsConciseMethod(function_kind_) || IsClassConstructor(function_kind_) ||
      IsAccessorFunction(function_kind_)) {
    this_function_ =
        Declare(zone(), this, ast_value_factory->this_function_string(), CONST,
                Variable::NORMAL, kCreatedInitialized);
  }
}

Variable* DeclarationScope::DeclareFunctionVar(const AstRawString* name) {
  DCHECK(is_function_scope());
  DCHECK_NULL(function_);
  VariableMode mode = is_strict(language_mode()) ? CONST : CONST_LEGACY;
  function_ = new (zone())
      Variable(this, name, mode, Variable::NORMAL, kCreatedInitialized);
  return function_;
}

Scope* Scope::FinalizeBlockScope() {
  DCHECK(is_block_scope());

  if (variables_.occupancy() > 0 ||
      (is_declaration_scope() && calls_sloppy_eval())) {
    return this;
  }

  // Remove this scope from outer scope.
  outer_scope()->RemoveInnerScope(this);

  // Reparent inner scopes.
  if (inner_scope_ != nullptr) {
    Scope* scope = inner_scope_;
    scope->outer_scope_ = outer_scope();
    while (scope->sibling_ != nullptr) {
      scope = scope->sibling_;
      scope->outer_scope_ = outer_scope();
    }
    scope->sibling_ = outer_scope()->inner_scope_;
    outer_scope()->inner_scope_ = inner_scope_;
    inner_scope_ = nullptr;
  }

  // Move unresolved variables
  if (unresolved_ != nullptr) {
    if (outer_scope()->unresolved_ != nullptr) {
      VariableProxy* unresolved = unresolved_;
      while (unresolved->next_unresolved() != nullptr) {
        unresolved = unresolved->next_unresolved();
      }
      unresolved->set_next_unresolved(outer_scope()->unresolved_);
    }
    outer_scope()->unresolved_ = unresolved_;
    unresolved_ = nullptr;
  }

  PropagateUsageFlagsToScope(outer_scope_);
  // This block does not need a context.
  num_heap_slots_ = 0;
  return NULL;
}

void Scope::Snapshot::Reparent(DeclarationScope* new_parent) const {
  DCHECK_EQ(new_parent, outer_scope_->inner_scope_);
  DCHECK_EQ(new_parent->outer_scope_, outer_scope_);
  DCHECK_EQ(new_parent, new_parent->GetClosureScope());
  DCHECK_NULL(new_parent->inner_scope_);
  DCHECK_NULL(new_parent->unresolved_);
  DCHECK_EQ(0, new_parent->locals_.length());
  Scope* inner_scope = new_parent->sibling_;
  if (inner_scope != top_inner_scope_) {
    for (; inner_scope->sibling() != top_inner_scope_;
         inner_scope = inner_scope->sibling()) {
      inner_scope->outer_scope_ = new_parent;
      DCHECK_NE(inner_scope, new_parent);
    }
    inner_scope->outer_scope_ = new_parent;

    new_parent->inner_scope_ = new_parent->sibling_;
    inner_scope->sibling_ = nullptr;
    // Reset the sibling rather than the inner_scope_ since we
    // want to keep new_parent there.
    new_parent->sibling_ = top_inner_scope_;
  }

  if (outer_scope_->unresolved_ != top_unresolved_) {
    VariableProxy* last = outer_scope_->unresolved_;
    while (last->next_unresolved() != top_unresolved_) {
      last = last->next_unresolved();
    }
    last->set_next_unresolved(nullptr);
    new_parent->unresolved_ = outer_scope_->unresolved_;
    outer_scope_->unresolved_ = top_unresolved_;
  }

  // TODO(verwaest): This currently only moves do-expression declared variables
  // in default arguments that weren't already previously declared with the same
  // name in the closure-scope. See
  // test/mjsunit/harmony/default-parameter-do-expression.js.
  DeclarationScope* outer_closure = outer_scope_->GetClosureScope();
  for (int i = top_local_; i < outer_closure->locals_.length(); i++) {
    Variable* local = outer_closure->locals_.at(i);
    DCHECK(local->mode() == TEMPORARY || local->mode() == VAR);
    DCHECK_EQ(local->scope(), local->scope()->GetClosureScope());
    DCHECK_NE(local->scope(), new_parent);
    local->set_scope(new_parent);
    new_parent->AddLocal(local);
    if (local->mode() == VAR) {
      outer_closure->variables_.Remove(local);
      new_parent->variables_.Add(new_parent->zone(), local);
    }
  }
  outer_closure->locals_.Rewind(top_local_);
  outer_closure->decls_.Rewind(top_decl_);
}

void Scope::ReplaceOuterScope(Scope* outer) {
  DCHECK_NOT_NULL(outer);
  DCHECK_NOT_NULL(outer_scope_);
  DCHECK(!already_resolved_);
  DCHECK(!outer->already_resolved_);
  DCHECK(!outer_scope_->already_resolved_);
  outer_scope_->RemoveInnerScope(this);
  outer->AddInnerScope(this);
  outer_scope_ = outer;
}


void Scope::PropagateUsageFlagsToScope(Scope* other) {
  DCHECK_NOT_NULL(other);
  DCHECK(!already_resolved_);
  DCHECK(!other->already_resolved_);
  if (calls_eval()) other->RecordEvalCall();
}

Variable* Scope::LookupInScopeInfo(const AstRawString* name) {
  Handle<String> name_handle = name->string();
  // The Scope is backed up by ScopeInfo. This means it cannot operate in a
  // heap-independent mode, and all strings must be internalized immediately. So
  // it's ok to get the Handle<String> here.
  // If we have a serialized scope info, we might find the variable there.
  // There should be no local slot with the given name.
  DCHECK_LT(scope_info_->StackSlotIndex(*name_handle), 0);

  VariableMode mode;
  InitializationFlag init_flag;
  MaybeAssignedFlag maybe_assigned_flag;

  VariableLocation location = VariableLocation::CONTEXT;
  int index = ScopeInfo::ContextSlotIndex(scope_info_, name_handle, &mode,
                                          &init_flag, &maybe_assigned_flag);
  if (index < 0 && scope_type() == MODULE_SCOPE) {
    location = VariableLocation::MODULE;
    index = -1;  // TODO(neis): Find module variables in scope info.
  }
  if (index < 0) return nullptr;  // Nowhere found.

  Variable::Kind kind = Variable::NORMAL;
  if (location == VariableLocation::CONTEXT &&
      index == scope_info_->ReceiverContextSlotIndex()) {
    kind = Variable::THIS;
  }
  // TODO(marja, rossberg): Correctly declare FUNCTION, CLASS, NEW_TARGET, and
  // ARGUMENTS bindings as their corresponding Variable::Kind.

  Variable* var = variables_.Declare(zone(), this, name, mode, kind, init_flag,
                                     maybe_assigned_flag);
  var->AllocateTo(location, index);
  return var;
}

Variable* DeclarationScope::LookupFunctionVar(const AstRawString* name) {
  if (function_ != nullptr && function_->raw_name() == name) {
    return function_;
  } else if (!scope_info_.is_null()) {
    // If we are backed by a scope info, try to lookup the variable there.
    VariableMode mode;
    int index = scope_info_->FunctionContextSlotIndex(*(name->string()), &mode);
    if (index < 0) return nullptr;
    Variable* var = DeclareFunctionVar(name);
    DCHECK_EQ(mode, var->mode());
    var->AllocateTo(VariableLocation::CONTEXT, index);
    return var;
  } else {
    return nullptr;
  }
}


Variable* Scope::Lookup(const AstRawString* name) {
  for (Scope* scope = this;
       scope != NULL;
       scope = scope->outer_scope()) {
    Variable* var = scope->LookupLocal(name);
    if (var != NULL) return var;
  }
  return NULL;
}

Variable* DeclarationScope::DeclareParameter(
    const AstRawString* name, VariableMode mode, bool is_optional, bool is_rest,
    bool* is_duplicate, AstValueFactory* ast_value_factory) {
  DCHECK(!already_resolved_);
  DCHECK(is_function_scope());
  DCHECK(!has_rest_);
  DCHECK(!is_optional || !is_rest);
  Variable* var;
  if (mode == TEMPORARY) {
    var = NewTemporary(name);
  } else {
    var = Declare(zone(), this, name, mode, Variable::NORMAL,
                  kCreatedInitialized);
    // TODO(wingo): Avoid O(n^2) check.
    *is_duplicate = IsDeclaredParameter(name);
  }
  if (!is_optional && !is_rest && arity_ == params_.length()) {
    ++arity_;
  }
  has_rest_ = is_rest;
  params_.Add(var, zone());
  if (name == ast_value_factory->arguments_string()) {
    has_arguments_parameter_ = true;
  }
  return var;
}

Variable* Scope::DeclareLocal(const AstRawString* name, VariableMode mode,
                              InitializationFlag init_flag, Variable::Kind kind,
                              MaybeAssignedFlag maybe_assigned_flag) {
  DCHECK(!already_resolved_);
  // This function handles VAR, LET, and CONST modes.  DYNAMIC variables are
  // introduced during variable allocation, and TEMPORARY variables are
  // allocated via NewTemporary().
  DCHECK(IsDeclaredVariableMode(mode));
  return Declare(zone(), this, name, mode, kind, init_flag,
                 maybe_assigned_flag);
}

Variable* Scope::DeclareVariable(
    Declaration* declaration, VariableMode mode, InitializationFlag init,
    bool allow_harmony_restrictive_generators,
    bool* sloppy_mode_block_scope_function_redefinition, bool* ok) {
  DCHECK(IsDeclaredVariableMode(mode) && mode != CONST_LEGACY);
  DCHECK(!already_resolved_);

  if (mode == VAR && !is_declaration_scope()) {
    return GetDeclarationScope()->DeclareVariable(
        declaration, mode, init, allow_harmony_restrictive_generators,
        sloppy_mode_block_scope_function_redefinition, ok);
  }
  DCHECK(!is_catch_scope());
  DCHECK(!is_with_scope());
  DCHECK(is_declaration_scope() ||
         (IsLexicalVariableMode(mode) && is_block_scope()));

  VariableProxy* proxy = declaration->proxy();
  DCHECK(proxy->raw_name() != NULL);
  const AstRawString* name = proxy->raw_name();
  bool is_function_declaration = declaration->IsFunctionDeclaration();

  Variable* var = nullptr;
  if (is_eval_scope() && is_sloppy(language_mode()) && mode == VAR) {
    // In a var binding in a sloppy direct eval, pollute the enclosing scope
    // with this new binding by doing the following:
    // The proxy is bound to a lookup variable to force a dynamic declaration
    // using the DeclareEvalVar or DeclareEvalFunction runtime functions.
    Variable::Kind kind = Variable::NORMAL;
    // TODO(sigurds) figure out if kNotAssigned is OK here
    var = new (zone()) Variable(this, name, mode, kind, init, kNotAssigned);
    var->AllocateTo(VariableLocation::LOOKUP, -1);
  } else {
    // Declare the variable in the declaration scope.
    var = LookupLocal(name);
    if (var == NULL) {
      // Declare the name.
      Variable::Kind kind = Variable::NORMAL;
      if (is_function_declaration) {
        kind = Variable::FUNCTION;
      }
      var = DeclareLocal(name, mode, init, kind, kNotAssigned);
    } else if (IsLexicalVariableMode(mode) ||
               IsLexicalVariableMode(var->mode())) {
      // Allow duplicate function decls for web compat, see bug 4693.
      bool duplicate_allowed = false;
      if (is_sloppy(language_mode()) && is_function_declaration &&
          var->is_function()) {
        DCHECK(IsLexicalVariableMode(mode) &&
               IsLexicalVariableMode(var->mode()));
        // If the duplication is allowed, then the var will show up
        // in the SloppyBlockFunctionMap and the new FunctionKind
        // will be a permitted duplicate.
        FunctionKind function_kind =
            declaration->AsFunctionDeclaration()->fun()->kind();
        duplicate_allowed =
            GetDeclarationScope()->sloppy_block_function_map()->Lookup(
                const_cast<AstRawString*>(name), name->hash()) != nullptr &&
            !IsAsyncFunction(function_kind) &&
            !(allow_harmony_restrictive_generators &&
              IsGeneratorFunction(function_kind));
      }
      if (duplicate_allowed) {
        *sloppy_mode_block_scope_function_redefinition = true;
      } else {
        // The name was declared in this scope before; check for conflicting
        // re-declarations. We have a conflict if either of the declarations
        // is not a var (in script scope, we also have to ignore legacy const
        // for compatibility). There is similar code in runtime.cc in the
        // Declare functions. The function CheckConflictingVarDeclarations
        // checks for var and let bindings from different scopes whereas this
        // is a check for conflicting declarations within the same scope. This
        // check also covers the special case
        //
        // function () { let x; { var x; } }
        //
        // because the var declaration is hoisted to the function scope where
        // 'x' is already bound.
        DCHECK(IsDeclaredVariableMode(var->mode()));
        // In harmony we treat re-declarations as early errors. See
        // ES5 16 for a definition of early errors.
        *ok = false;
        return nullptr;
      }
    } else if (mode == VAR) {
      var->set_maybe_assigned();
    }
  }
  DCHECK_NOT_NULL(var);

  // We add a declaration node for every declaration. The compiler
  // will only generate code if necessary. In particular, declarations
  // for inner local variables that do not represent functions won't
  // result in any generated code.
  //
  // This will lead to multiple declaration nodes for the
  // same variable if it is declared several times. This is not a
  // semantic issue, but it may be a performance issue since it may
  // lead to repeated DeclareEvalVar or DeclareEvalFunction calls.
  decls_.Add(declaration, zone());
  proxy->BindTo(var);
  return var;
}

Variable* DeclarationScope::DeclareDynamicGlobal(const AstRawString* name,
                                                 Variable::Kind kind) {
  DCHECK(is_script_scope());
  return variables_.Declare(zone(), this, name, DYNAMIC_GLOBAL, kind,
                            kCreatedInitialized);
}


bool Scope::RemoveUnresolved(VariableProxy* var) {
  if (unresolved_ == var) {
    unresolved_ = var->next_unresolved();
    var->set_next_unresolved(nullptr);
    return true;
  }
  VariableProxy* current = unresolved_;
  while (current != nullptr) {
    VariableProxy* next = current->next_unresolved();
    if (var == next) {
      current->set_next_unresolved(next->next_unresolved());
      var->set_next_unresolved(nullptr);
      return true;
    }
    current = next;
  }
  return false;
}


Variable* Scope::NewTemporary(const AstRawString* name) {
  DeclarationScope* scope = GetClosureScope();
  Variable* var = new(zone()) Variable(scope,
                                       name,
                                       TEMPORARY,
                                       Variable::NORMAL,
                                       kCreatedInitialized);
  scope->AddLocal(var);
  return var;
}

Declaration* Scope::CheckConflictingVarDeclarations() {
  int length = decls_.length();
  for (int i = 0; i < length; i++) {
    Declaration* decl = decls_[i];
    VariableMode mode = decl->proxy()->var()->mode();
    if (IsLexicalVariableMode(mode) && !is_block_scope()) continue;

    // Iterate through all scopes until and including the declaration scope.
    Scope* previous = NULL;
    Scope* current = decl->scope();
    // Lexical vs lexical conflicts within the same scope have already been
    // captured in Parser::Declare. The only conflicts we still need to check
    // are lexical vs VAR, or any declarations within a declaration block scope
    // vs lexical declarations in its surrounding (function) scope.
    if (IsLexicalVariableMode(mode)) current = current->outer_scope_;
    do {
      // There is a conflict if there exists a non-VAR binding.
      Variable* other_var =
          current->variables_.Lookup(decl->proxy()->raw_name());
      if (other_var != NULL && IsLexicalVariableMode(other_var->mode())) {
        return decl;
      }
      previous = current;
      current = current->outer_scope_;
    } while (!previous->is_declaration_scope());
  }
  return NULL;
}

Declaration* Scope::CheckLexDeclarationsConflictingWith(
    const ZoneList<const AstRawString*>& names) {
  DCHECK(is_block_scope());
  for (int i = 0; i < names.length(); ++i) {
    Variable* var = LookupLocal(names.at(i));
    if (var != nullptr) {
      // Conflict; find and return its declaration.
      DCHECK(IsLexicalVariableMode(var->mode()));
      const AstRawString* name = names.at(i);
      for (int j = 0; j < decls_.length(); ++j) {
        if (decls_[j]->proxy()->raw_name() == name) {
          return decls_[j];
        }
      }
      DCHECK(false);
    }
  }
  return nullptr;
}

void DeclarationScope::AllocateVariables(ParseInfo* info, bool for_debugger) {
  PropagateScopeInfo();
  ResolveVariablesRecursively(info);
  AllocateVariablesRecursively();
  AllocateScopeInfosRecursively(info->isolate(), for_debugger);
}

bool Scope::AllowsLazyParsing() const {
  // If we are inside a block scope, we must parse eagerly to find out how
  // to allocate variables on the block scope. At this point, declarations may
  // not have yet been parsed.
  for (const Scope* s = this; s != nullptr; s = s->outer_scope_) {
    if (s->is_block_scope()) return false;
  }
  return true;
}

bool DeclarationScope::AllowsLazyCompilation() const {
  return !force_eager_compilation_;
}

bool DeclarationScope::AllowsLazyCompilationWithoutContext() const {
  if (force_eager_compilation_) return false;
  // Disallow lazy compilation without context if any outer scope needs a
  // context.
  for (const Scope* scope = outer_scope_; scope != nullptr;
       scope = scope->outer_scope_) {
    if (scope->NeedsContext()) return false;
  }
  return true;
}

int Scope::ContextChainLength(Scope* scope) const {
  int n = 0;
  for (const Scope* s = this; s != scope; s = s->outer_scope_) {
    DCHECK(s != NULL);  // scope must be in the scope chain
    if (s->NeedsContext()) n++;
  }
  return n;
}

int Scope::ContextChainLengthUntilOutermostSloppyEval() const {
  int result = 0;
  int length = 0;

  for (const Scope* s = this; s != nullptr; s = s->outer_scope()) {
    if (!s->NeedsContext()) continue;
    length++;
    if (s->calls_sloppy_eval()) result = length;
  }

  return result;
}

int Scope::MaxNestedContextChainLength() {
  int max_context_chain_length = 0;
  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    max_context_chain_length = std::max(scope->MaxNestedContextChainLength(),
                                        max_context_chain_length);
  }
  if (NeedsContext()) {
    max_context_chain_length += 1;
  }
  return max_context_chain_length;
}

DeclarationScope* Scope::GetDeclarationScope() {
  Scope* scope = this;
  while (!scope->is_declaration_scope()) {
    scope = scope->outer_scope();
  }
  return scope->AsDeclarationScope();
}

DeclarationScope* Scope::GetClosureScope() {
  Scope* scope = this;
  while (!scope->is_declaration_scope() || scope->is_block_scope()) {
    scope = scope->outer_scope();
  }
  return scope->AsDeclarationScope();
}

DeclarationScope* Scope::GetReceiverScope() {
  Scope* scope = this;
  while (!scope->is_script_scope() &&
         (!scope->is_function_scope() ||
          scope->AsDeclarationScope()->is_arrow_scope())) {
    scope = scope->outer_scope();
  }
  return scope->AsDeclarationScope();
}

Handle<StringSet> DeclarationScope::CollectNonLocals(
    ParseInfo* info, Handle<StringSet> non_locals) {
  VariableProxy* free_variables = FetchFreeVariables(this, info);
  for (VariableProxy* proxy = free_variables; proxy != nullptr;
       proxy = proxy->next_unresolved()) {
    non_locals = StringSet::Add(non_locals, proxy->name());
  }
  return non_locals;
}

void DeclarationScope::AnalyzePartially(DeclarationScope* migrate_to,
                                        AstNodeFactory* ast_node_factory) {
  // Gather info from inner scopes.
  PropagateScopeInfo();

  // Try to resolve unresolved variables for this Scope and migrate those which
  // cannot be resolved inside. It doesn't make sense to try to resolve them in
  // the outer Scopes here, because they are incomplete.
  for (VariableProxy* proxy = FetchFreeVariables(this); proxy != nullptr;
       proxy = proxy->next_unresolved()) {
    DCHECK(!proxy->is_resolved());
    VariableProxy* copy = ast_node_factory->CopyVariableProxy(proxy);
    migrate_to->AddUnresolved(copy);
  }

  // Push scope data up to migrate_to. Note that migrate_to and this Scope
  // describe the same Scope, just in different Zones.
  PropagateUsageFlagsToScope(migrate_to);
  if (scope_uses_super_property_) migrate_to->scope_uses_super_property_ = true;
  if (inner_scope_calls_eval_) migrate_to->inner_scope_calls_eval_ = true;
  DCHECK(!force_eager_compilation_);
  migrate_to->set_start_position(start_position_);
  migrate_to->set_end_position(end_position_);
  migrate_to->set_language_mode(language_mode());
  migrate_to->arity_ = arity_;
  migrate_to->force_context_allocation_ = force_context_allocation_;
  outer_scope_->RemoveInnerScope(this);
  DCHECK_EQ(outer_scope_, migrate_to->outer_scope_);
  DCHECK_EQ(outer_scope_->zone(), migrate_to->zone());
  DCHECK_EQ(NeedsHomeObject(), migrate_to->NeedsHomeObject());
  DCHECK_EQ(asm_function_, migrate_to->asm_function_);
  DCHECK_EQ(arguments() != nullptr, migrate_to->arguments() != nullptr);
}

#ifdef DEBUG
static const char* Header(ScopeType scope_type, FunctionKind function_kind,
                          bool is_declaration_scope) {
  switch (scope_type) {
    case EVAL_SCOPE: return "eval";
    // TODO(adamk): Should we print concise method scopes specially?
    case FUNCTION_SCOPE:
      if (IsGeneratorFunction(function_kind)) return "function*";
      if (IsAsyncFunction(function_kind)) return "async function";
      if (IsArrowFunction(function_kind)) return "arrow";
      return "function";
    case MODULE_SCOPE: return "module";
    case SCRIPT_SCOPE: return "global";
    case CATCH_SCOPE: return "catch";
    case BLOCK_SCOPE: return is_declaration_scope ? "varblock" : "block";
    case WITH_SCOPE: return "with";
  }
  UNREACHABLE();
  return NULL;
}


static void Indent(int n, const char* str) {
  PrintF("%*s%s", n, "", str);
}


static void PrintName(const AstRawString* name) {
  PrintF("%.*s", name->length(), name->raw_data());
}


static void PrintLocation(Variable* var) {
  switch (var->location()) {
    case VariableLocation::UNALLOCATED:
      break;
    case VariableLocation::PARAMETER:
      PrintF("parameter[%d]", var->index());
      break;
    case VariableLocation::LOCAL:
      PrintF("local[%d]", var->index());
      break;
    case VariableLocation::CONTEXT:
      PrintF("context[%d]", var->index());
      break;
    case VariableLocation::GLOBAL:
      PrintF("global[%d]", var->index());
      break;
    case VariableLocation::LOOKUP:
      PrintF("lookup");
      break;
    case VariableLocation::MODULE:
      PrintF("module");
      break;
  }
}


static void PrintVar(int indent, Variable* var) {
  if (var->is_used() || !var->IsUnallocated()) {
    Indent(indent, Variable::Mode2String(var->mode()));
    PrintF(" ");
    if (var->raw_name()->IsEmpty())
      PrintF(".%p", reinterpret_cast<void*>(var));
    else
      PrintName(var->raw_name());
    PrintF(";  // ");
    PrintLocation(var);
    bool comma = !var->IsUnallocated();
    if (var->has_forced_context_allocation()) {
      if (comma) PrintF(", ");
      PrintF("forced context allocation");
      comma = true;
    }
    if (var->maybe_assigned() == kMaybeAssigned) {
      if (comma) PrintF(", ");
      PrintF("maybe assigned");
    }
    PrintF("\n");
  }
}

static void PrintMap(int indent, VariableMap* map, bool locals) {
  for (VariableMap::Entry* p = map->Start(); p != nullptr; p = map->Next(p)) {
    Variable* var = reinterpret_cast<Variable*>(p->value);
    bool local = !IsDynamicVariableMode(var->mode());
    if (locals ? local : !local) {
      if (var == nullptr) {
        Indent(indent, "<?>\n");
      } else {
        PrintVar(indent, var);
      }
    }
  }
}

void DeclarationScope::PrintParameters() {
  PrintF(" (");
  for (int i = 0; i < params_.length(); i++) {
    if (i > 0) PrintF(", ");
    const AstRawString* name = params_[i]->raw_name();
    if (name->IsEmpty())
      PrintF(".%p", reinterpret_cast<void*>(params_[i]));
    else
      PrintName(name);
  }
  PrintF(")");
}

void Scope::Print(int n) {
  int n0 = (n > 0 ? n : 0);
  int n1 = n0 + 2;  // indentation

  // Print header.
  FunctionKind function_kind = is_function_scope()
                                   ? AsDeclarationScope()->function_kind()
                                   : kNormalFunction;
  Indent(n0, Header(scope_type_, function_kind, is_declaration_scope()));
  if (scope_name_ != nullptr && !scope_name_->IsEmpty()) {
    PrintF(" ");
    PrintName(scope_name_);
  }

  // Print parameters, if any.
  Variable* function = nullptr;
  if (is_function_scope()) {
    AsDeclarationScope()->PrintParameters();
    function = AsDeclarationScope()->function_var();
  }

  PrintF(" { // (%d, %d)\n", start_position(), end_position());

  // Function name, if any (named function literals, only).
  if (function != nullptr) {
    Indent(n1, "// (local) function name: ");
    PrintName(function->raw_name());
    PrintF("\n");
  }

  // Scope info.
  if (is_strict(language_mode())) {
    Indent(n1, "// strict mode scope\n");
  }
  if (IsAsmModule()) Indent(n1, "// scope is an asm module\n");
  if (IsAsmFunction()) Indent(n1, "// scope is an asm function\n");
  if (scope_calls_eval_) Indent(n1, "// scope calls 'eval'\n");
  if (is_declaration_scope() && AsDeclarationScope()->uses_super_property()) {
    Indent(n1, "// scope uses 'super' property\n");
  }
  if (inner_scope_calls_eval_) Indent(n1, "// inner scope calls 'eval'\n");
  if (num_stack_slots_ > 0) {
    Indent(n1, "// ");
    PrintF("%d stack slots\n", num_stack_slots_);
  }
  if (num_heap_slots_ > 0) {
    Indent(n1, "// ");
    PrintF("%d heap slots\n", num_heap_slots_);
  }

  // Print locals.
  if (function != nullptr) {
    Indent(n1, "// function var:\n");
    PrintVar(n1, function);
  }

  if (variables_.Start() != NULL) {
    Indent(n1, "// local vars:\n");
    PrintMap(n1, &variables_, true);

    Indent(n1, "// dynamic vars:\n");
    PrintMap(n1, &variables_, false);
  }

  // Print inner scopes (disable by providing negative n).
  if (n >= 0) {
    for (Scope* scope = inner_scope_; scope != nullptr;
         scope = scope->sibling_) {
      PrintF("\n");
      scope->Print(n1);
    }
  }

  Indent(n0, "}\n");
}

void Scope::CheckScopePositions() {
  // A scope is allowed to have invalid positions if it is hidden and has no
  // inner scopes
  if (!is_hidden() && inner_scope_ == nullptr) {
    CHECK_NE(kNoSourcePosition, start_position());
    CHECK_NE(kNoSourcePosition, end_position());
  }
  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    scope->CheckScopePositions();
  }
}

void Scope::CheckZones() {
  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    CHECK_EQ(scope->zone(), zone());
  }
}
#endif  // DEBUG

Variable* Scope::NonLocal(const AstRawString* name, VariableMode mode) {
  // Declare a new non-local.
  DCHECK(IsDynamicVariableMode(mode));
  Variable* var = variables_.Declare(zone(), NULL, name, mode, Variable::NORMAL,
                                     kCreatedInitialized);
  // Allocate it by giving it a dynamic lookup.
  var->AllocateTo(VariableLocation::LOOKUP, -1);
  return var;
}

Variable* Scope::LookupRecursive(VariableProxy* proxy, bool declare_free,
                                 Scope* outer_scope_end) {
  DCHECK_NE(outer_scope_end, this);
  // Short-cut: whenever we find a debug-evaluate scope, just look everything up
  // dynamically. Debug-evaluate doesn't properly create scope info for the
  // lookups it does. It may not have a valid 'this' declaration, and anything
  // accessed through debug-evaluate might invalidly resolve to stack-allocated
  // variables.
  // TODO(yangguo): Remove once debug-evaluate creates proper ScopeInfo for the
  // scopes in which it's evaluating.
  if (is_debug_evaluate_scope_) {
    if (!declare_free) return nullptr;
    return NonLocal(proxy->raw_name(), DYNAMIC);
  }

  // Try to find the variable in this scope.
  Variable* var = LookupLocal(proxy->raw_name());

  // We found a variable and we are done. (Even if there is an 'eval' in this
  // scope which introduces the same variable again, the resulting variable
  // remains the same.)
  if (var != nullptr) return var;

  // We did not find a variable locally. Check against the function variable, if
  // any.
  if (is_function_scope()) {
    var = AsDeclarationScope()->LookupFunctionVar(proxy->raw_name());
    if (var != nullptr) {
      if (calls_sloppy_eval()) return NonLocal(proxy->raw_name(), DYNAMIC);
      return var;
    }
  }

  if (outer_scope_ == outer_scope_end) {
    if (!declare_free) return nullptr;
    DCHECK(is_script_scope());
    // No binding has been found. Declare a variable on the global object.
    return AsDeclarationScope()->DeclareDynamicGlobal(proxy->raw_name(),
                                                      Variable::NORMAL);
  }

  DCHECK(!is_script_scope());

  var = outer_scope_->LookupRecursive(proxy, declare_free, outer_scope_end);

  // The variable could not be resolved statically.
  if (var == nullptr) return var;

  if (is_function_scope() && !var->is_dynamic()) {
    var->ForceContextAllocation();
  }
  // "this" can't be shadowed by "eval"-introduced bindings or by "with"
  // scopes.
  // TODO(wingo): There are other variables in this category; add them.
  if (var->is_this()) return var;

  if (is_with_scope()) {
    // The current scope is a with scope, so the variable binding can not be
    // statically resolved. However, note that it was necessary to do a lookup
    // in the outer scope anyway, because if a binding exists in an outer
    // scope, the associated variable has to be marked as potentially being
    // accessed from inside of an inner with scope (the property may not be in
    // the 'with' object).
    if (!var->is_dynamic() && var->IsUnallocated()) {
      DCHECK(!already_resolved_);
      var->set_is_used();
      var->ForceContextAllocation();
      if (proxy->is_assigned()) var->set_maybe_assigned();
    }
    return NonLocal(proxy->raw_name(), DYNAMIC);
  }

  if (calls_sloppy_eval() && is_declaration_scope()) {
    // A variable binding may have been found in an outer scope, but the current
    // scope makes a sloppy 'eval' call, so the found variable may not be the
    // correct one (the 'eval' may introduce a binding with the same name). In
    // that case, change the lookup result to reflect this situation. Only
    // scopes that can host var bindings (declaration scopes) need be considered
    // here (this excludes block and catch scopes), and variable lookups at
    // script scope are always dynamic.
    if (var->IsGlobalObjectProperty()) {
      return NonLocal(proxy->raw_name(), DYNAMIC_GLOBAL);
    }

    if (var->is_dynamic()) return var;

    Variable* invalidated = var;
    var = NonLocal(proxy->raw_name(), DYNAMIC_LOCAL);
    var->set_local_if_not_shadowed(invalidated);
  }

  return var;
}

void Scope::ResolveVariable(ParseInfo* info, VariableProxy* proxy) {
  DCHECK(info->script_scope()->is_script_scope());

  // If the proxy is already resolved there's nothing to do
  // (functions and consts may be resolved by the parser).
  if (proxy->is_resolved()) return;

  // Otherwise, try to resolve the variable.
  Variable* var = LookupRecursive(proxy, true, nullptr);

  ResolveTo(info, proxy, var);
}

void Scope::ResolveTo(ParseInfo* info, VariableProxy* proxy, Variable* var) {
#ifdef DEBUG
  if (info->script_is_native()) {
    // To avoid polluting the global object in native scripts
    //  - Variables must not be allocated to the global scope.
    CHECK_NOT_NULL(outer_scope());
    //  - Variables must be bound locally or unallocated.
    if (var->IsGlobalObjectProperty()) {
      // The following variable name may be minified. If so, disable
      // minification in js2c.py for better output.
      Handle<String> name = proxy->raw_name()->string();
      V8_Fatal(__FILE__, __LINE__, "Unbound variable: '%s' in native script.",
               name->ToCString().get());
    }
    VariableLocation location = var->location();
    CHECK(location == VariableLocation::LOCAL ||
          location == VariableLocation::CONTEXT ||
          location == VariableLocation::PARAMETER ||
          location == VariableLocation::UNALLOCATED);
  }
#endif

  DCHECK_NOT_NULL(var);
  if (proxy->is_assigned()) var->set_maybe_assigned();
  proxy->BindTo(var);
}

void Scope::ResolveVariablesRecursively(ParseInfo* info) {
  DCHECK(info->script_scope()->is_script_scope());

  // Resolve unresolved variables for this scope.
  for (VariableProxy* proxy = unresolved_; proxy != nullptr;
       proxy = proxy->next_unresolved()) {
    ResolveVariable(info, proxy);
  }

  // Resolve unresolved variables for inner scopes.
  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    scope->ResolveVariablesRecursively(info);
  }
}

VariableProxy* Scope::FetchFreeVariables(DeclarationScope* max_outer_scope,
                                         ParseInfo* info,
                                         VariableProxy* stack) {
  for (VariableProxy *proxy = unresolved_, *next = nullptr; proxy != nullptr;
       proxy = next) {
    next = proxy->next_unresolved();
    if (proxy->is_resolved()) continue;
    Variable* var =
        LookupRecursive(proxy, false, max_outer_scope->outer_scope());
    if (var == nullptr) {
      proxy->set_next_unresolved(stack);
      stack = proxy;
    } else if (info != nullptr) {
      ResolveTo(info, proxy, var);
    }
  }

  // Clear unresolved_ as it's in an inconsistent state.
  unresolved_ = nullptr;

  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    stack = scope->FetchFreeVariables(max_outer_scope, info, stack);
  }

  return stack;
}

void Scope::PropagateScopeInfo() {
  for (Scope* inner = inner_scope_; inner != nullptr; inner = inner->sibling_) {
    inner->PropagateScopeInfo();
    if (IsAsmModule() && inner->is_function_scope()) {
      inner->AsDeclarationScope()->set_asm_function();
    }
  }
}


bool Scope::MustAllocate(Variable* var) {
  DCHECK(var->location() != VariableLocation::MODULE);
  // Give var a read/write use if there is a chance it might be accessed
  // via an eval() call.  This is only possible if the variable has a
  // visible name.
  if ((var->is_this() || !var->raw_name()->IsEmpty()) &&
      (inner_scope_calls_eval_ || is_catch_scope() || is_script_scope())) {
    var->set_is_used();
    if (inner_scope_calls_eval_) var->set_maybe_assigned();
  }
  DCHECK(!var->has_forced_context_allocation() || var->is_used());
  // Global variables do not need to be allocated.
  return !var->IsGlobalObjectProperty() && var->is_used();
}


bool Scope::MustAllocateInContext(Variable* var) {
  // If var is accessed from an inner scope, or if there is a possibility
  // that it might be accessed from the current or an inner scope (through
  // an eval() call or a runtime with lookup), it must be allocated in the
  // context.
  //
  // Exceptions: If the scope as a whole has forced context allocation, all
  // variables will have context allocation, even temporaries.  Otherwise
  // temporary variables are always stack-allocated.  Catch-bound variables are
  // always context-allocated.
  if (has_forced_context_allocation()) return true;
  if (var->mode() == TEMPORARY) return false;
  if (is_catch_scope()) return true;
  if (is_script_scope() && IsLexicalVariableMode(var->mode())) return true;
  return var->has_forced_context_allocation() || inner_scope_calls_eval_;
}


void Scope::AllocateStackSlot(Variable* var) {
  if (is_block_scope()) {
    outer_scope()->GetDeclarationScope()->AllocateStackSlot(var);
  } else {
    var->AllocateTo(VariableLocation::LOCAL, num_stack_slots_++);
  }
}


void Scope::AllocateHeapSlot(Variable* var) {
  var->AllocateTo(VariableLocation::CONTEXT, num_heap_slots_++);
}

void DeclarationScope::AllocateParameterLocals() {
  DCHECK(is_function_scope());

  bool uses_sloppy_arguments = false;

  // Functions have 'arguments' declared implicitly in all non arrow functions.
  if (arguments_ != nullptr) {
    // 'arguments' is used. Unless there is also a parameter called
    // 'arguments', we must be conservative and allocate all parameters to
    // the context assuming they will be captured by the arguments object.
    // If we have a parameter named 'arguments', a (new) value is always
    // assigned to it via the function invocation. Then 'arguments' denotes
    // that specific parameter value and cannot be used to access the
    // parameters, which is why we don't need to allocate an arguments
    // object in that case.
    if (MustAllocate(arguments_) && !has_arguments_parameter_) {
      // In strict mode 'arguments' does not alias formal parameters.
      // Therefore in strict mode we allocate parameters as if 'arguments'
      // were not used.
      // If the parameter list is not simple, arguments isn't sloppy either.
      uses_sloppy_arguments =
          is_sloppy(language_mode()) && has_simple_parameters();
    } else {
      // 'arguments' is unused. Tell the code generator that it does not need to
      // allocate the arguments object by nulling out arguments_.
      arguments_ = nullptr;
    }

  } else {
    DCHECK(is_arrow_scope());
  }

  // The same parameter may occur multiple times in the parameters_ list.
  // If it does, and if it is not copied into the context object, it must
  // receive the highest parameter index for that parameter; thus iteration
  // order is relevant!
  for (int i = num_parameters() - 1; i >= 0; --i) {
    Variable* var = params_[i];
    DCHECK(!has_rest_ || var != rest_parameter());
    DCHECK_EQ(this, var->scope());
    if (uses_sloppy_arguments) {
      var->ForceContextAllocation();
    }
    AllocateParameter(var, i);
  }
}

void DeclarationScope::AllocateParameter(Variable* var, int index) {
  if (MustAllocate(var)) {
    if (MustAllocateInContext(var)) {
      DCHECK(var->IsUnallocated() || var->IsContextSlot());
      if (var->IsUnallocated()) {
        AllocateHeapSlot(var);
      }
    } else {
      DCHECK(var->IsUnallocated() || var->IsParameter());
      if (var->IsUnallocated()) {
        var->AllocateTo(VariableLocation::PARAMETER, index);
      }
    }
  }
}

void DeclarationScope::AllocateReceiver() {
  if (!has_this_declaration()) return;
  DCHECK_NOT_NULL(receiver());
  DCHECK_EQ(receiver()->scope(), this);
  AllocateParameter(receiver(), -1);
}

void Scope::AllocateNonParameterLocal(Variable* var) {
  DCHECK(var->scope() == this);
  if (var->IsUnallocated() && MustAllocate(var)) {
    if (MustAllocateInContext(var)) {
      AllocateHeapSlot(var);
    } else {
      AllocateStackSlot(var);
    }
  }
}

void Scope::AllocateNonParameterLocalsAndDeclaredGlobals() {
  for (int i = 0; i < locals_.length(); i++) {
    AllocateNonParameterLocal(locals_[i]);
  }

  if (is_declaration_scope()) {
    AsDeclarationScope()->AllocateLocals();
  }
}

void DeclarationScope::AllocateLocals() {
  // For now, function_ must be allocated at the very end.  If it gets
  // allocated in the context, it must be the last slot in the context,
  // because of the current ScopeInfo implementation (see
  // ScopeInfo::ScopeInfo(FunctionScope* scope) constructor).
  if (function_ != nullptr) {
    AllocateNonParameterLocal(function_);
  }

  DCHECK(!has_rest_ || !MustAllocate(rest_parameter()) ||
         !rest_parameter()->IsUnallocated());

  if (new_target_ != nullptr && !MustAllocate(new_target_)) {
    new_target_ = nullptr;
  }

  if (this_function_ != nullptr && !MustAllocate(this_function_)) {
    this_function_ = nullptr;
  }
}

void ModuleScope::AllocateModuleVariables() {
  for (const auto& it : module()->regular_imports()) {
    Variable* var = LookupLocal(it.first);
    // TODO(neis): Use a meaningful index.
    var->AllocateTo(VariableLocation::MODULE, 42);
  }

  for (const auto& it : module()->regular_exports()) {
    Variable* var = LookupLocal(it.first);
    var->AllocateTo(VariableLocation::MODULE, 0);
  }
}

void Scope::AllocateVariablesRecursively() {
  DCHECK(!already_resolved_);
  DCHECK_EQ(0, num_stack_slots_);

  // Allocate variables for inner scopes.
  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    scope->AllocateVariablesRecursively();
  }

  DCHECK(!already_resolved_);
  DCHECK_EQ(Context::MIN_CONTEXT_SLOTS, num_heap_slots_);

  // Allocate variables for this scope.
  // Parameters must be allocated first, if any.
  if (is_declaration_scope()) {
    if (is_module_scope()) {
      AsModuleScope()->AllocateModuleVariables();
    } else if (is_function_scope()) {
      AsDeclarationScope()->AllocateParameterLocals();
    }
    AsDeclarationScope()->AllocateReceiver();
  }
  AllocateNonParameterLocalsAndDeclaredGlobals();

  // Force allocation of a context for this scope if necessary. For a 'with'
  // scope and for a function scope that makes an 'eval' call we need a context,
  // even if no local variables were statically allocated in the scope.
  // Likewise for modules.
  bool must_have_context =
      is_with_scope() || is_module_scope() ||
      (is_function_scope() && calls_sloppy_eval()) ||
      (is_block_scope() && is_declaration_scope() && calls_sloppy_eval());

  // If we didn't allocate any locals in the local context, then we only
  // need the minimal number of slots if we must have a context.
  if (num_heap_slots_ == Context::MIN_CONTEXT_SLOTS && !must_have_context) {
    num_heap_slots_ = 0;
  }

  // Allocation done.
  DCHECK(num_heap_slots_ == 0 || num_heap_slots_ >= Context::MIN_CONTEXT_SLOTS);
}

void Scope::AllocateScopeInfosRecursively(Isolate* isolate, bool for_debugger) {
  DCHECK(scope_info_.is_null());
  if (for_debugger || NeedsScopeInfo()) {
    scope_info_ = ScopeInfo::Create(isolate, zone(), this);
  }

  // Allocate ScopeInfos for inner scopes.
  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    scope->AllocateScopeInfosRecursively(isolate, for_debugger);
  }
}

int Scope::StackLocalCount() const {
  Variable* function =
      is_function_scope() ? AsDeclarationScope()->function_var() : nullptr;
  return num_stack_slots() -
         (function != nullptr && function->IsStackLocal() ? 1 : 0);
}


int Scope::ContextLocalCount() const {
  if (num_heap_slots() == 0) return 0;
  Variable* function =
      is_function_scope() ? AsDeclarationScope()->function_var() : nullptr;
  bool is_function_var_in_context =
      function != nullptr && function->IsContextSlot();
  return num_heap_slots() - Context::MIN_CONTEXT_SLOTS -
         (is_function_var_in_context ? 1 : 0);
}

}  // namespace internal
}  // namespace v8
