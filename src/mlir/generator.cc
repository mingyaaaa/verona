// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include "generator.h"

#include "ast-utils.h"
#include "dialect/VeronaDialect.h"
#include "dialect/VeronaOps.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/IR/Types.h"

#include "llvm/ADT/StringSwitch.h"

namespace
{
  /// Helper to make sure the basic block has a terminator
  bool hasTerminator(mlir::Block* bb)
  {
    return !bb->getOperations().empty() && bb->back().isKnownTerminator();
  }

  /// Add a new basic block into a region and return it
  mlir::Block* addBlock(mlir::Region* region)
  {
    region->push_back(new mlir::Block());
    return &region->back();
  }
}

namespace mlir::verona
{
  // FIXME: Until we decide how the interface is going to be, this helps to
  // keep the idea of separation without actually doing it. We could have just
  // a namespace and functions, a static class, or a stateful class, all of
  // which will have different choices on the calls to the interface.
  using namespace ASTInterface;

  // ===================================================== Public Interface
  llvm::Expected<mlir::OwningModuleRef>
  Generator::lower(MLIRContext* context, const ::ast::Ast& ast)
  {
    Generator gen(context);
    if (auto err = gen.parseModule(ast))
      return std::move(err);

    return std::move(gen.module);
  }

  // ===================================================== AST -> MLIR
  mlir::Location Generator::getLocation(const ::ast::Ast& ast)
  {
    auto path = getPath(ast);
    return builder.getFileLineColLoc(
      Identifier::get(path.file, context), path.line, path.column);
  }

  llvm::Error Generator::parseModule(const ::ast::Ast& ast)
  {
    assert(isClass(ast) && "Bad node");
    module = mlir::ModuleOp::create(getLocation(ast));
    // TODO: Support more than just functions at the module level
    auto body = getClassBody(ast);
    llvm::SmallVector<::ast::WeakAst, 4> funcs;
    getSubNodes(funcs, body);
    for (auto f : funcs)
    {
      auto fun = parseFunction(f.lock());
      if (auto err = fun.takeError())
        return err;
      module->push_back(*fun);
    }
    return llvm::Error::success();
  }

  llvm::Expected<mlir::FuncOp> Generator::parseProto(const ::ast::Ast& ast)
  {
    assert(isFunction(ast) && "Bad node");
    auto name = getFunctionName(ast);
    // We only care about the functions declared in the current scope
    if (functionTable.inScope(name))
      return functionTable.lookup(name);

    // Parse 'where' clause
    llvm::SmallVector<::ast::WeakAst, 4> constraints;
    getFunctionConstraints(constraints, ast);
    for (auto c : constraints)
    {
      // This is wrong. Constraints are not aliases, but with
      // the oversimplified representaiton we have and the fluid
      // state of the type system, this will "work" for now.
      auto alias = getID(c);
      auto ty = getType(c);
      typeTable.insert(alias, parseType(ty.lock()));
    }

    // Function type from signature
    Types types;
    llvm::SmallVector<::ast::WeakAst, 4> args;
    getFunctionArgs(args, ast);
    for (auto arg : args)
      types.push_back(parseType(getType(arg).lock()));
    auto retTy = parseType(getFunctionType(ast).lock());
    auto funcTy = builder.getFunctionType(types, retTy);

    // Create function
    auto func = mlir::FuncOp::create(getLocation(ast), name, funcTy);
    functionTable.insert(name, func);
    return func;
  }

  llvm::Expected<mlir::FuncOp> Generator::parseFunction(const ::ast::Ast& ast)
  {
    assert(isFunction(ast) && "Bad node");

    // Declare function signature
    TypeScopeT alias_scope(typeTable);
    auto name = getFunctionName(ast);
    auto proto = parseProto(ast);
    if (auto err = proto.takeError())
      return std::move(err);
    auto& func = *proto;
    auto retTy = func.getType().getResult(0);

    // Create entry block
    auto& entryBlock = *func.addEntryBlock();
    builder.setInsertionPointToStart(&entryBlock);

    // Declare all arguments on current scope
    SymbolScopeT var_scope(symbolTable);
    llvm::SmallVector<::ast::WeakAst, 4> args;
    getFunctionArgs(args, ast);
    auto argVals = entryBlock.getArguments();
    assert(args.size() == argVals.size() && "Argument mismatch");
    for (auto var_val : llvm::zip(args, argVals))
    {
      // Get the argument name/value
      auto name = getID(std::get<0>(var_val).lock());
      auto value = std::get<1>(var_val);
      // Allocate space in the stack
      auto alloca =
        genOperation(getLocation(ast), "verona.alloca", {}, allocaTy);
      if (auto err = alloca.takeError())
        return std::move(err);
      // Store the value of the argument
      auto store = genOperation(
        getLocation(ast), "verona.store", {value, alloca->get()}, unkTy);
      if (auto err = store.takeError())
        return std::move(err);
      // Associate the name with the alloca SSA value
      declareVariable(name, alloca->get());
    }

    // Lower body
    auto body = getFunctionBody(ast);
    auto last = parseNode(body.lock());
    if (auto err = last.takeError())
      return std::move(err);

    // Check if needs to return a value at all
    if (hasTerminator(builder.getBlock()))
      return func;

    // Return last value (or none)
    // TODO: Implement multiple return values for tuples
    if (last->hasValue() && last->get().getType() != retTy)
    {
      // Cast type (we trust the ast)
      last =
        genOperation(last->get().getLoc(), "verona.cast", {last->get()}, retTy);
    }
    else
    {
      // None type (void)
      // TODO: We should declare void functions without a return type
      last = genOperation(getLocation(ast), "verona.none", {}, retTy);
    }
    if (auto err = last.takeError())
      return std::move(err);
    builder.create<mlir::ReturnOp>(getLocation(ast), last->getAll());

    return func;
  }

  mlir::Type Generator::parseType(const ::ast::Ast& ast)
  {
    assert(isType(ast) && "Bad node");
    auto desc = getTypeDesc(ast);
    if (desc.empty())
      return builder.getNoneType();

    // If type is in the alias table, get it
    auto type = typeTable.lookup(desc);
    if (type)
      return type;

    // Else, insert into the table and return
    type = genOpaqueType(desc);
    typeTable.insert(desc, type);
    return type;
  }

  void Generator::declareVariable(llvm::StringRef name, mlir::Value val)
  {
    assert(!symbolTable.inScope(name) && "Redeclaration");
    symbolTable.insert(name, val);
  }

  void Generator::updateVariable(llvm::StringRef name, mlir::Value val)
  {
    assert(symbolTable.inScope(name) && "Variable not declared");
    symbolTable.update(name, val);
  }

  llvm::Expected<ReturnValue> Generator::parseBlock(const ::ast::Ast& ast)
  {
    ReturnValue last;
    llvm::SmallVector<::ast::WeakAst, 4> nodes;
    getSubNodes(nodes, ast);
    for (auto sub : nodes)
    {
      auto node = parseNode(sub.lock());
      if (auto err = node.takeError())
        return std::move(err);
      last = *node;
    }
    return last;
  }

  llvm::Expected<ReturnValue> Generator::parseNode(const ::ast::Ast& ast)
  {
    switch (getKind(ast))
    {
      case NodeKind::Localref:
        return parseValue(ast);
      case NodeKind::Block:
      case NodeKind::Seq:
        return parseBlock(ast);
      case NodeKind::ID:
        return parseValue(ast);
      case NodeKind::Assign:
        return parseAssign(ast);
      case NodeKind::Call:
        return parseCall(ast);
      case NodeKind::Return:
        return parseReturn(ast);
      case NodeKind::If:
        return parseCondition(ast);
      case NodeKind::While:
        return parseWhileLoop(ast);
      case NodeKind::Continue:
        return parseContinue(ast);
      case NodeKind::Break:
        return parseBreak(ast);
    }

    if (isValue(ast))
      return parseValue(ast);

    return parsingError(
      "Node " + getName(ast) + " not implemented yet", getLocation(ast));
  }

  llvm::Expected<ReturnValue> Generator::parseValue(const ::ast::Ast& ast)
  {
    // Variables
    if (isLocalRef(ast))
    {
      // We use allocas to track location and load/stores to track access
      auto name = getTokenValue(ast);
      auto var = symbolTable.lookup(name);
      assert(var && "Undeclared variable lookup, broken ast");
      if (var.getType() == allocaTy)
        return genOperation(getLocation(ast), "verona.load", {var}, unkTy);
      return var;
    }

    // Constants
    if (isConstant(ast))
    {
      // We lower each constant to their own values for now as we
      // don't yet have a good scheme for the types and MLIR can't
      // have attributes from unknown types. Once we set on a type
      // system compatibility between Verona and MLIR, we can change
      // this to emit the attribute right away.
      auto type = genOpaqueType(getName(ast));
      auto value = getTokenValue(ast);
      return genOperation(
        getLocation(ast), "verona.constant(" + value + ")", {}, type);
    }

    // TODO: Literals need attributes and types
    assert(isValue(ast) && "Bad node");
    return parsingError(
      "Value [" + getName(ast) + " = " + getTokenValue(ast) +
        "] not implemented yet",
      getLocation(ast));
  }

  llvm::Expected<ReturnValue> Generator::parseAssign(const ::ast::Ast& ast)
  {
    assert(isAssign(ast) && "Bad node");

    // Can either be a let (new variable) or localref (existing variable).
    auto var = getLHS(ast);
    auto name = getLocalName(var);

    // If the variable wasn't declared yet in this context, create an alloca
    // TODO: Implement declaration of tuples (multiple values)
    if (isLet(var))
    {
      auto alloca =
        genOperation(getLocation(ast), "verona.alloca", {}, allocaTy);
      if (auto err = alloca.takeError())
        return std::move(err);
      declareVariable(name, alloca->get());
    }
    auto store = symbolTable.lookup(name);
    if (!store)
      return parsingError(
        "Variable " + name + " not declared before use",
        getLocation(var.lock()));

    // The right-hand side can be any expression
    // This is the value and we update the variable
    auto rhs = parseNode(getRHS(ast).lock());
    if (auto err = rhs.takeError())
      return std::move(err);

    // Store the value in the alloca
    auto op = genOperation(
      getLocation(ast), "verona.store", {rhs->get(), store}, unkTy);
    if (auto err = op.takeError())
      return std::move(err);
    return store;
  }

  llvm::Expected<ReturnValue> Generator::parseCall(const ::ast::Ast& ast)
  {
    assert(isCall(ast) && "Bad node");
    auto name = getID(ast);

    // All operations are calls, only calls to previously defined functions
    // are function calls.
    if (auto func = functionTable.lookup(name))
    {
      llvm::SmallVector<::ast::WeakAst, 4> argNodes;
      getAllOperands(argNodes, ast);
      assert(
        argNodes.size() == func.getNumArguments() &&
        "Wrong number of arguments");
      llvm::SmallVector<mlir::Value, 4> args;
      for (const auto& val_ty : llvm::zip(argNodes, func.getArguments()))
      {
        auto arg = std::get<0>(val_ty).lock();
        auto val = parseNode(arg);
        if (auto err = val.takeError())
          return std::move(err);

        // Types are incomplete here, so add casts (will be cleaned later)
        auto argTy = std::get<1>(val_ty).getType();
        auto cast =
          genOperation(getLocation(arg), "verona.cast", {val->get()}, argTy);
        if (auto err = cast.takeError())
          return std::move(err);

        args.push_back(cast->get());
      }

      auto call = builder.create<mlir::CallOp>(getLocation(ast), func, args);
      auto res = call.getResults();
      return res;
    }

    // Else, it should be an operation that we can lower natively
    if (isUnary(ast))
    {
      return parsingError(
        "Unary Operation '" + name + "' not implemented yet", getLocation(ast));
    }
    else if (isBinary(ast))
    {
      // Get both arguments
      // TODO: If the arguments are tuples, do we need to apply element-wise?
      auto arg0 = parseNode(getOperand(ast, 0).lock());
      if (auto err = arg0.takeError())
        return std::move(err);
      auto arg1 = parseNode(getOperand(ast, 1).lock());
      if (auto err = arg1.takeError())
        return std::move(err);

      // Get op name and type
      using opPairTy = std::pair<llvm::StringRef, mlir::Type>;
      opPairTy op = llvm::StringSwitch<opPairTy>(name)
                      .Case("+", {"verona.add", unkTy})
                      .Case("-", {"verona.sub", unkTy})
                      .Case("*", {"verona.mul", unkTy})
                      .Case("/", {"verona.div", unkTy})
                      .Case("==", {"verona.eq", boolTy})
                      .Case("!=", {"verona.ne", boolTy})
                      .Case(">", {"verona.gt", boolTy})
                      .Case("<", {"verona.lt", boolTy})
                      .Case(">=", {"verona.ge", boolTy})
                      .Case("<=", {"verona.le", boolTy})
                      .Default(std::make_pair("", unkTy));

      // Match, return the right op with the right type
      if (!op.first.empty())
        return genOperation(
          getLocation(ast), op.first, {arg0->get(), arg1->get()}, op.second);

      return parsingError(
        "Binary operation '" + name + "' not implemented yet",
        getLocation(ast));
    }

    return parsingError(
      "Operation '" + name + "' not implemented yet", getLocation(ast));
  }

  llvm::Expected<ReturnValue> Generator::parseReturn(const ::ast::Ast& ast)
  {
    assert(isReturn(ast) && "Bad node");
    auto expr = parseNode(getSingleSubNode(ast).lock());
    if (auto err = expr.takeError())
      return std::move(err);

    // Check which type of region we're in
    auto region = builder.getInsertionBlock()->getParent();
    mlir::Type retTy;
    auto op = region->getParentOp();
    bool isInLoop = isa<mlir::verona::WhileOp>(op);

    // Get the function return type (for casts)
    while (!isa<mlir::FuncOp>(op))
    {
      op = op->getParentRegion()->getParentOp();
    }
    if (auto func = dyn_cast<mlir::FuncOp>(op))
    {
      retTy = func.getType().getResult(0);
    }
    else
    {
      return parsingError(
        "Return operation without parent function", getLocation(ast));
    }

    // Emit the cast if necessary
    if (expr->hasValue() && expr->get().getType() != retTy)
    {
      // Cast type (we trust the ast)
      expr =
        genOperation(expr->get().getLoc(), "verona.cast", {expr->get()}, retTy);
      if (auto err = expr.takeError())
        return std::move(err);
    }

    // Emit the right return instruction
    if (isInLoop)
    {
      builder.create<mlir::verona::LoopReturnOp>(
        getLocation(ast), expr->getAll());
    }
    else
    {
      builder.create<mlir::ReturnOp>(getLocation(ast), expr->getAll());
    }

    // No values to return, basic block is terminated.
    return ReturnValue();
  }

  llvm::Expected<ReturnValue> Generator::parseCondition(const ::ast::Ast& ast)
  {
    assert(isIf(ast) && "Bad node");

    // TODO: MLIR doesn't support conditions with literals
    // we need to make a constexpr decision and only lower the right block
    if (isConstant(getCond(ast)))
    {
      return parsingError(
        "Conditionals with literals not supported yet", getLocation(ast));
    }

    // Create local context for condition variables (valid for both if/else)
    SymbolScopeT var_scope{symbolTable};

    // First node is a sequence of conditions
    // lower in the current basic block.
    auto condNode = getCond(ast).lock();
    auto condLoc = getLocation(condNode);
    auto cond = parseNode(condNode);
    if (auto err = cond.takeError())
      return std::move(err);

    // Create basic-blocks, conditionally branch to if/else
    auto region = builder.getInsertionBlock()->getParent();
    mlir::ValueRange empty{};
    auto ifBB = addBlock(region);
    mlir::Block* elseBB = nullptr;
    if (hasElse(ast))
      elseBB = addBlock(region);
    auto exitBB = addBlock(region);
    if (hasElse(ast))
    {
      builder.create<mlir::CondBranchOp>(
        condLoc, cond->get(), ifBB, empty, elseBB, empty);
    }
    else
    {
      builder.create<mlir::CondBranchOp>(
        condLoc, cond->get(), ifBB, empty, exitBB, empty);
    }

    {
      // Create local context for the if block variables
      SymbolScopeT if_scope{symbolTable};

      // If block
      auto ifNode = getIfBlock(ast).lock();
      auto ifLoc = getLocation(ifNode);
      builder.setInsertionPointToEnd(ifBB);
      auto ifBlock = parseNode(ifNode);
      if (auto err = ifBlock.takeError())
        return std::move(err);
      if (!hasTerminator(builder.getBlock()))
        builder.create<mlir::BranchOp>(ifLoc, exitBB, empty);
    }

    // Else block
    // We don't need to lower the else part if it's empty
    if (hasElse(ast))
    {
      // Create local context for the else block variables
      SymbolScopeT else_scope{symbolTable};

      auto elseNode = getElseBlock(ast).lock();
      auto elseLoc = getLocation(elseNode);
      builder.setInsertionPointToEnd(elseBB);
      auto elseBlock = parseNode(elseNode);
      if (auto err = elseBlock.takeError())
        return std::move(err);
      if (!hasTerminator(builder.getBlock()))
        builder.create<mlir::BranchOp>(elseLoc, exitBB, empty);
    }

    // Move to exit block, where the remaining instructions will be lowered.
    builder.setInsertionPointToEnd(exitBB);

    // No values to return from lexical constructs.
    return ReturnValue();
  }

  llvm::Expected<ReturnValue> Generator::parseWhileLoop(const ::ast::Ast& ast)
  {
    assert(isWhile(ast) && "Bad node");

    // TODO: MLIR doesn't support conditions with literals
    // we need to make a constexpr decision and only lower the right block
    if (isConstant(getCond(ast)))
    {
      return parsingError(
        "Loop conditions with literals not supported yet", getLocation(ast));
    }

    // Create local context for loop variables
    SymbolScopeT var_scope{symbolTable};

    // While operation
    auto whileOp = builder.create<mlir::verona::WhileOp>(getLocation(ast));
    auto block = addBlock(&whileOp.body());

    // First node is a sequence of conditions for the loop_exit node
    builder.setInsertionPointToEnd(block);
    auto condNode = getCond(ast).lock();
    auto cond = parseNode(condNode);
    if (auto err = cond.takeError())
      return std::move(err);
    builder.create<mlir::verona::LoopExitOp>(
      getLocation(condNode), cond->get());

    // Loop body, may create more BBs, must terminate (default: continue)
    auto bodyNode = getLoopBlock(ast).lock();
    auto bodyBlock = parseNode(bodyNode);
    if (auto err = bodyBlock.takeError())
      return std::move(err);
    if (!hasTerminator(builder.getBlock()))
      builder.create<mlir::verona::ContinueOp>(getLocation(bodyNode));

    // Continue on the contained basic block, after the while
    builder.setInsertionPointAfter(whileOp);

    // No values to return from lexical constructs.
    return ReturnValue();
  }

  llvm::Expected<ReturnValue> Generator::parseContinue(const ::ast::Ast& ast)
  {
    assert(isContinue(ast) && "Bad node");
    builder.create<mlir::verona::ContinueOp>(getLocation(ast));

    // No values to return, basic block is terminated.
    return ReturnValue();
  }

  // Can we merge this code with the function above?
  llvm::Expected<ReturnValue> Generator::parseBreak(const ::ast::Ast& ast)
  {
    assert(isBreak(ast) && "Bad node");
    builder.create<mlir::verona::BreakOp>(getLocation(ast));

    // No values to return, basic block is terminated.
    return ReturnValue();
  }

  llvm::Expected<ReturnValue> Generator::genOperation(
    mlir::Location loc,
    llvm::StringRef name,
    llvm::ArrayRef<mlir::Value> ops,
    mlir::Type retTy)
  {
    auto opName = OperationName(name, context);
    auto state = OperationState(loc, opName);
    state.addOperands(ops);
    state.addTypes({retTy});
    auto op = builder.createOperation(state);
    auto res = op->getResults();
    return res;
  }

  mlir::OpaqueType Generator::genOpaqueType(llvm::StringRef name)
  {
    auto dialect = mlir::Identifier::get("type", context);
    return mlir::OpaqueType::get(dialect, name, context);
  }
}
