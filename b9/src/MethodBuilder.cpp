#include "b9/compiler/MethodBuilder.hpp"
#include "b9/ExecutionContext.hpp"
#include "b9/JitHelpers.hpp"
#include "b9/VirtualMachine.hpp"
#include "b9/compiler/Compiler.hpp"
#include "b9/instructions.hpp"

#include <ilgen/VirtualMachineOperandStack.hpp>
#include <ilgen/VirtualMachineRegister.hpp>
#include <ilgen/VirtualMachineRegisterInStruct.hpp>


namespace b9 {

namespace ValueBuilder {

TR::IlValue *getInt48(TR::BytecodeBuilder *builder, TR::IlValue *value) {
  return builder->And(value, builder->ConstInt64(Om::Value::PAYLOAD_MASK));
}

TR::IlValue *fromInt48(TR::BytecodeBuilder *builder, TR::IlValue *value) {
  return builder->Or(value, builder->ConstInt64(Om::Value::Tag::INT48));
}

}  // namespace ValueBuilder

MethodBuilder::MethodBuilder(VirtualMachine &virtualMachine,
                             const std::size_t functionIndex)
    : TR::MethodBuilder(&virtualMachine.compiler()->typeDictionary()),
      virtualMachine_(virtualMachine),
      cfg_(virtualMachine.config()),
      maxInlineDepth_(cfg_.maxInlineDepth),
      globalTypes_(virtualMachine.compiler()->globalTypes()),
      functionIndex_(functionIndex) {
  const FunctionDef *function = virtualMachine_.getFunction(functionIndex);

  /// TODO: The __LINE__/__FILE__ stuff is 100% bogus, this is about as bad.
  DefineLine("<unknown");
  DefineFile(function->name.c_str());

  DefineName(function->name.c_str());

  DefineReturnType(globalTypes().stackElement);

  defineParameters();

  defineLocals();

  defineFunctions();

  AllLocalsHaveBeenDefined();
}

static const char *argsAndTempNames[] = {
    "arg00", "arg01", "arg02", "arg03", "arg04", "arg05", "arg06",
    "arg07", "arg08", "arg09", "arg10", "arg11", "arg12", "arg13",
    "arg14", "arg15", "arg16", "arg17", "arg18", "arg19", "arg20",
    "arg21", "arg22", "arg23", "arg24", "arg25", "arg26", "arg27",
    "arg28", "arg29", "arg30", "arg31", "arg32"};
#define MAX_ARGS_TEMPS_AVAIL \
  (sizeof(argsAndTempNames) / sizeof(argsAndTempNames[0]))

/// The first argument is always executionContext.
/// The remaining function arguments are only passed as native arguments in
/// PassParam mode.
void MethodBuilder::defineParameters() {
  const FunctionDef *function = virtualMachine_.getFunction(functionIndex_);
  if (cfg_.debug) {
    std::cout << "Defining " << function->nparams << " parameters\n";
  }

  /// first argument is always the execution context
  DefineParameter("executionContext", globalTypes().executionContextPtr);

  if (cfg_.passParam) {
    for (int i = 0; i < function->nparams; i++) {
      DefineParameter(argsAndTempNames[i], globalTypes().stackElement);
    }
  }
}

void MethodBuilder::defineLocals() {
  const FunctionDef *function = virtualMachine_.getFunction(functionIndex_);
  if (cfg_.debug) {
    std::cout << "Defining " << function->nlocals << " locals\n";
  }

  // Pointer to the base of the frame.  Used for rolling back the stack pointer
  // on a return.
  DefineLocal("stackBase", globalTypes().stackElementPtr);

  // Pointer to the operand stack
  DefineLocal("stack", globalTypes().operandStackPtr);

  // Address of the current stack top
  DefineLocal("stackTop", globalTypes().stackElementPtr);

  if (cfg_.passParam) {
    // for locals we pre-define all the locals we could use, for the toplevel
    // and all the inlined names which are simply referenced via a skew to reach
    // past callers functions args/temps
    for (std::size_t i = function->nparams;
         i < (function->nlocals + function->nparams); i++) {
      DefineLocal(argsAndTempNames[i], globalTypes().stackElement);
    }
  }
}

void MethodBuilder::defineFunctions() {
  int functionIndex = 0;
  while (functionIndex < virtualMachine_.getFunctionCount()) {
    if (virtualMachine_.getJitAddress(functionIndex) != nullptr) {
      auto function = virtualMachine_.getFunction(functionIndex);
      auto name = function->name.c_str();
      DefineFunction(name, (char *)__FILE__, name,
                     (void *)virtualMachine_.getJitAddress(functionIndex),
                     Int64, function->nparams, globalTypes().stackElement,
                     globalTypes().stackElement, globalTypes().stackElement,
                     globalTypes().stackElement, globalTypes().stackElement,
                     globalTypes().stackElement, globalTypes().stackElement,
                     globalTypes().stackElement);
    }
    functionIndex++;
  }

  DefineFunction((char *)"interpret0", (char *)__FILE__, "interpret0",
                 (void *)&interpret0, Int64, 2,
                 globalTypes().executionContextPtr, globalTypes().int32Ptr);
  DefineFunction((char *)"interpret1", (char *)__FILE__, "interpret1",
                 (void *)&interpret1, Int64, 3,
                 globalTypes().executionContextPtr, globalTypes().int32Ptr,
                 globalTypes().stackElement);
  DefineFunction((char *)"interpret2", (char *)__FILE__, "interpret2",
                 (void *)&interpret2, Int64, 4,
                 globalTypes().executionContextPtr, globalTypes().int32Ptr,
                 globalTypes().stackElement, globalTypes().stackElement);
  DefineFunction((char *)"interpret3", (char *)__FILE__, "interpret3",
                 (void *)&interpret3, Int64, 5,
                 globalTypes().executionContextPtr, globalTypes().int32Ptr,
                 globalTypes().stackElement, globalTypes().stackElement,
                 globalTypes().stackElement);
  DefineFunction((char *)"primitive_call", (char *)__FILE__, "primitive_call",
                 (void *)&primitive_call, NoType, 2,
                 globalTypes().executionContextPtr, Int32);
  DefineFunction((char *)"trace", (char *)__FILE__, "trace", (void *)&trace,
                 NoType, 2, globalTypes().addressPtr, globalTypes().addressPtr);
  DefineFunction((char *)"print_stack", (char *)__FILE__, "print_stack",
                 (void *)&print_stack, NoType, 1,
                 globalTypes().executionContextPtr);
}

#define QSTACK(b) (((VirtualMachineState *)(b)->vmState())->_stack)
#define QRELOAD(b) \
  if (cfg_.lazyVmState) ((b)->vmState()->Reload(b));
#define QRELOAD_DROP(b, toDrop) \
  if (cfg_.lazyVmState) QSTACK(b)->Drop(b, toDrop);

bool MethodBuilder::inlineProgramIntoBuilder(
    const std::size_t functionIndex, bool isTopLevel,
    TR::BytecodeBuilder *currentBuilder,
    TR::BytecodeBuilder *jumpToBuilderForInlinedReturn) {
  bool success = true;
  maxInlineDepth_--;
  const FunctionDef *function = virtualMachine_.getFunction(functionIndex);
  const Instruction *program = function->instructions.data();

  // Create a BytecodeBuilder for each Bytecode
  auto numberOfBytecodes = function->instructions.size();

  if (numberOfBytecodes == 0) {
    if (cfg_.debug) {
      std::cerr << "unexpected EMPTY function body for " << function->name
                << std::endl;
    }
    return false;
  }

  if (cfg_.debug)
    std::cout << "Creating " << numberOfBytecodes << " bytecode builders"
              << std::endl;

  // create the builders

  std::vector<TR::BytecodeBuilder *> builderTable;
  builderTable.reserve(numberOfBytecodes);
  for (int i = 0; i < numberOfBytecodes; i++) {
    builderTable.push_back(OrphanBytecodeBuilder(i));
  }

  // Get the first Builder

  TR::BytecodeBuilder *builder = builderTable[0];

  if (isTopLevel) {
    AppendBuilder(builder);
  } else {
    currentBuilder->AddFallThroughBuilder(builder);
  }

  // Gen IL
  bool ok = true;

  for (std::size_t index = GetNextBytecodeFromWorklist(); index != -1;
       index = GetNextBytecodeFromWorklist()) {
    ok = generateILForBytecode(function, builderTable, index,
                               jumpToBuilderForInlinedReturn);
    if (!ok) break;
  }
  return ok;
}

bool MethodBuilder::buildIL() {
  const FunctionDef *function = virtualMachine_.getFunction(functionIndex_);

  TR::IlValue *stack = StructFieldInstanceAddress(
      "b9::ExecutionContext", "stack_", Load("executionContext"));
  Store("stack", stack);

  TR::IlValue *stackTop = LoadIndirect("b9::OperandStack", "top_", stack);
  Store("stackTop", stackTop);

  if (cfg_.lazyVmState) {
    auto simulatedStackTop = new OMR::VirtualMachineRegisterInStruct(
        this, "b9::OperandStack", "stack", "top_", "stackTop");

    auto simulateOperandStack = new OMR::VirtualMachineOperandStack(
        this, 64 /* starting size */, globalTypes().stackElement,
        simulatedStackTop, true /* grows up */, 0 /* commit address offset */);

    auto vms = new VirtualMachineState(simulateOperandStack, simulatedStackTop);

    setVMState(vms);

  } else {
    setVMState(new OMR::VirtualMachineState());
  }

  /// When this function exits, we reset the stack top to the beginning of
  /// entry. The calling convention is callee-cleanup, so at exit we pop all
  /// the args off the operand stack.
  ///
  /// In the case of pass immediate, the arguments are not passed on the VM
  /// stack. The arguments are passed on the C stack as a part of a cdecl call.
  if (!cfg_.passParam) {
    TR::IlValue *stackBase = IndexAt(globalTypes().stackElementPtr, stackTop,
                                     ConstInt32(-function->nparams));
    Store("stackBase", stackBase);
  } else {
    Store("stackBase", stackTop);
  }

  // Locals are stored on the stack. Bump the stackTop by the number of
  // registers/locals in the function.
  //
  // In the case of passParam, locals are not stored on the VM stack.  Local
  // variables are stored as compiler immidiets.
  if (!cfg_.passParam && function->nlocals > 0) {
    TR::IlValue *newStackTop = IndexAt(globalTypes().stackElementPtr, stackTop,
                                       ConstInt32(function->nlocals));

    StoreIndirect("b9::OperandStack", "top_", stack, newStackTop);
  }

  // initialize all locals to 0
  auto paramsCount = function->nparams;
  auto localsCount = function->nlocals;
  for (int i = paramsCount; i < paramsCount + localsCount; i++) {
    storeVal(this, i, this->ConstInt64(0));
  }

  return inlineProgramIntoBuilder(functionIndex_, true);
}

void MethodBuilder::storeVal(TR::IlBuilder *builder, int valIndex,
                             TR::IlValue *value) {
  if (firstArgumentIndex > 0) {
    valIndex += firstArgumentIndex;
  }

  if (cfg_.passParam) {
    builder->Store(argsAndTempNames[valIndex], value);
  } else {
    TR::IlValue *args = builder->Load("stackBase");
    TR::IlValue *address = builder->IndexAt(globalTypes().stackElementPtr, args,
                                            builder->ConstInt32(valIndex));
    builder->StoreAt(address, value);
  }
}

TR::IlValue *MethodBuilder::loadVal(TR::IlBuilder *builder, int valIndex) {
  if (firstArgumentIndex > 0) {
    valIndex += firstArgumentIndex;
  }

  if (cfg_.passParam) {
    return builder->Load(argsAndTempNames[valIndex]);
  } else {
    TR::IlValue *args = builder->Load("stackBase");
    TR::IlValue *address = builder->IndexAt(globalTypes().stackElementPtr, args,
                                            builder->ConstInt32(valIndex));
    TR::IlValue *result =
        builder->LoadAt(globalTypes().stackElementPtr, address);
    return result;
  }
}

TR::IlValue *MethodBuilder::loadLocalIndex(TR::IlBuilder *builder,
                                           int localIndex) {
  const FunctionDef *currentFunction =
      virtualMachine_.getFunction(functionIndex_);
  return loadVal(builder, localIndex + currentFunction->nparams);
}

void MethodBuilder::storeLocalIndex(TR::IlBuilder *builder, int localIndex,
                                    TR::IlValue *value) {
  const FunctionDef *currentFunction =
      virtualMachine_.getFunction(functionIndex_);
  storeVal(builder, localIndex + currentFunction->nparams, value);
}

TR::IlValue *MethodBuilder::loadParamIndex(TR::IlBuilder *builder,
                                           int paramIndex) {
  return loadVal(builder, paramIndex);
}

void MethodBuilder::storeParamIndex(TR::IlBuilder *builder, int paramIndex,
                                    TR::IlValue *value) {
  storeVal(builder, paramIndex, value);
}

bool MethodBuilder::generateILForBytecode(
    const FunctionDef *function,
    std::vector<TR::BytecodeBuilder *> bytecodeBuilderTable,
    std::size_t instructionIndex,
    TR::BytecodeBuilder *jumpToBuilderForInlinedReturn) {
  TR::BytecodeBuilder *builder = bytecodeBuilderTable[instructionIndex];
  const std::vector<Instruction> &program = function->instructions;
  const Instruction instruction = program[instructionIndex];

  if (cfg_.debug) {
    std::cout << "generating index=" << instructionIndex
              << " bc=" << instruction << std::endl;
  }

  if (nullptr == builder) {
    if (cfg_.debug)
      std::cout << "unexpected NULL BytecodeBuilder!" << std::endl;
    return false;
  }

  TR::BytecodeBuilder *nextBytecodeBuilder = nullptr;

  if (instructionIndex + 1 <= program.size()) {
    nextBytecodeBuilder = bytecodeBuilderTable[instructionIndex + 1];
  }

  bool handled = true;

  if (cfg_.debug) {
    if (jumpToBuilderForInlinedReturn != nullptr) {
      std::cout << "INLINED METHOD: skew " << firstArgumentIndex
                << " return bc will jump to " << jumpToBuilderForInlinedReturn
                << ": ";
    }

    builder->Call("print_stack", 1, builder->Load("executionContext"));

    builder->Call(
        "trace", 2, builder->ConstAddress(function),
        builder->ConstAddress(&function->instructions[instructionIndex]));

    builder->vmState()->Commit(builder);
  }

  switch (instruction.opCode()) {
    case OpCode::PUSH_FROM_LOCAL:
      pushValue(builder, loadLocalIndex(builder, instruction.immediate()));
      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
      break;
    case OpCode::POP_INTO_LOCAL:
      storeLocalIndex(builder, instruction.immediate(), popValue(builder));
      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
      break;
    case OpCode::PUSH_FROM_PARAM:
      pushValue(builder, loadParamIndex(builder, instruction.immediate()));
      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
      break;
    case OpCode::POP_INTO_PARAM:
      storeParamIndex(builder, instruction.immediate(), popValue(builder));
      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
      break;
    case OpCode::FUNCTION_RETURN: {
      auto result = popValue(builder);

      TR::IlValue *stack = builder->StructFieldInstanceAddress(
          "b9::ExecutionContext", "stack_", builder->Load("executionContext"));

      builder->StoreIndirect("b9::OperandStack", "top_", stack,
                             builder->Load("stackBase"));

      builder->Return(
          builder->Or(result, builder->ConstInt64(Om::Value::Tag::INT48)));
    } break;
    case OpCode::DUPLICATE: {
      auto x = popValue(builder);
      pushValue(builder, x);
      pushValue(builder, x);
      if (nextBytecodeBuilder) {
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
      }
    } break;
    case OpCode::DROP:
      drop(builder);
      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
      break;
    case OpCode::JMP:
      handle_bc_jmp(builder, bytecodeBuilderTable, program, instructionIndex,
                    nextBytecodeBuilder);
      break;
    case OpCode::INT_JMP_EQ:
      handle_bc_jmp_eq(builder, bytecodeBuilderTable, program, instructionIndex,
                       nextBytecodeBuilder);
      break;
    case OpCode::INT_JMP_NEQ:
      handle_bc_jmp_neq(builder, bytecodeBuilderTable, program,
                        instructionIndex, nextBytecodeBuilder);
      break;
    case OpCode::INT_JMP_LT:
      handle_bc_jmp_lt(builder, bytecodeBuilderTable, program, instructionIndex,
                       nextBytecodeBuilder);
      break;
    case OpCode::INT_JMP_LE:
      handle_bc_jmp_le(builder, bytecodeBuilderTable, program, instructionIndex,
                       nextBytecodeBuilder);
      break;
    case OpCode::INT_JMP_GT:
      handle_bc_jmp_gt(builder, bytecodeBuilderTable, program, instructionIndex,
                       nextBytecodeBuilder);
      break;
    case OpCode::INT_JMP_GE:
      handle_bc_jmp_ge(builder, bytecodeBuilderTable, program, instructionIndex,
                       nextBytecodeBuilder);
      break;
    case OpCode::INT_SUB:
      handle_bc_sub(builder, nextBytecodeBuilder);
      break;
    case OpCode::INT_ADD:
      handle_bc_add(builder, nextBytecodeBuilder);
      break;
    case OpCode::INT_MUL:
      handle_bc_mul(builder, nextBytecodeBuilder);
      break;
    case OpCode::INT_DIV:
      handle_bc_div(builder, nextBytecodeBuilder);
      break;
    case OpCode::INT_NOT:
      handle_bc_not(builder, nextBytecodeBuilder);
      break;
    case OpCode::INT_PUSH_CONSTANT: {
      int constvalue = instruction.immediate();
      /// TODO: box/unbox here.
      pushInt48(builder, builder->ConstInt64(constvalue));
      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
    } break;
    case OpCode::STR_PUSH_CONSTANT: {
      int index = instruction.immediate();
      /// TODO: Box/unbox here.
      pushInt48(builder, builder->ConstInt64(index));
      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
    } break;
    case OpCode::PRIMITIVE_CALL: {
      builder->vmState()->Commit(builder);
      TR::IlValue *result =
          builder->Call("primitive_call", 2, builder->Load("executionContext"),
                        builder->ConstInt32(instruction.immediate()));
      QRELOAD(builder);
      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
    } break;
    case OpCode::FUNCTION_CALL: {
      const std::size_t callindex = instruction.immediate();
      const FunctionDef *callee = virtualMachine_.getFunction(callindex);
      const Instruction *tocall = callee->instructions.data();
      const std::uint32_t paramsCount = callee->nparams;
      const std::uint32_t localsCount = callee->nlocals;

      if (cfg_.directCall) {
        if (cfg_.debug)
          std::cout << "Handling direct calls to " << callee->name << std::endl;
        const char *interpretName[] = {"interpret0", "interpret1", "interpret2",
                                       "interpret3"};
        const char *nameToCall = interpretName[paramsCount];
        bool interp = true;
        if (callee == function ||
            virtualMachine_.getJitAddress(callindex) != nullptr) {
          nameToCall = callee->name.c_str();
          interp = false;
        }

        if (cfg_.passParam) {
          if (cfg_.debug) {
            std::cout << "Parameters are passed to the function call"
                      << std::endl;
          }

          // Attempt to inline the function we're calling
          if (maxInlineDepth_ >= 0 && !interp) {
            int32_t save = firstArgumentIndex;
            int32_t skipLocals = function->nparams + function->nlocals;
            int32_t spaceNeeded = paramsCount + localsCount;
            firstArgumentIndex += skipLocals;
            // no need to define locals here, the outer program registered
            // all locals. it means some locals will be reused which will
            // affect liveness of a variable
            if ((firstArgumentIndex + spaceNeeded) < MAX_ARGS_TEMPS_AVAIL) {
              int storeInto = paramsCount;
              while (storeInto-- > 0) {
                // firstArgumentIndex is added in storeVal
                storeParamIndex(builder, storeInto, popValue(builder));
              }

              bool result = inlineProgramIntoBuilder(callindex, false, builder,
                                                     nextBytecodeBuilder);
              if (!result) {
                std::cerr << "Failed inlineProgramIntoBuilder" << std::endl;
                return result;
              }

              if (cfg_.debug)
                std::cout << "Successfully inlined: " << callee->name
                          << std::endl;
              firstArgumentIndex = save;
              break;
            }
            std::cerr << "SKIP INLINE DUE TO EXCESSIVE TEMPS NEEDED"
                      << std::endl;
          }

          if (paramsCount > 8) {
            throw std::runtime_error{
                "Need to add handlers for more parameters"};
            break;
          }

          TR::IlValue *p[8];
          memset(p, 0, sizeof(p));
          int popInto = paramsCount;
          while (popInto--) {
            p[popInto] = popValue(builder);
          }
          if (interp) {
            TR::IlValue *result = builder->Call(
                nameToCall, 2 + paramsCount, builder->Load("executionContext"),
                builder->ConstInt32(callindex), p[0], p[1], p[2], p[3], p[4],
                p[5], p[6], p[7]);
            pushValue(builder, result);
          } else {
            TR::IlValue *result = builder->Call(
                nameToCall, paramsCount + 1, builder->Load("executionContext"),
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            pushValue(builder, result);
          }
        } else {
          if (cfg_.debug) {
            std::cout << "Parameters are on stack to the function call"
                      << std::endl;
          }
          TR::IlValue *result;
          builder->vmState()->Commit(builder);
          if (interp) {
            if (cfg_.debug)
              std::cout << "calling interpreter: interpret0" << std::endl;
            result = builder->Call("interpret0", 2,
                                   builder->Load("executionContext"),
                                   builder->ConstInt32(callindex));
          } else {
            if (cfg_.debug)
              std::cout << "calling " << nameToCall << " directly" << std::endl;
            result =
                builder->Call(nameToCall, 1, builder->Load("executionContext"));
          }
          QRELOAD_DROP(builder, paramsCount);
          pushValue(builder, result);
        }
      } else {
        // only use interpreter to dispatch the calls
        if (cfg_.debug)
          std::cout << "Calling interpret0 to dispatch call for "
                    << callee->name << " with " << paramsCount << " args"
                    << std::endl;
        builder->vmState()->Commit(builder);
        TR::IlValue *result =
            builder->Call("interpret0", 2, builder->Load("executionContext"),
                          builder->ConstInt32(callindex));
        QRELOAD_DROP(builder, paramsCount);
        pushValue(builder, result);
      }

      if (nextBytecodeBuilder)
        builder->AddFallThroughBuilder(nextBytecodeBuilder);
    } break;
    default:
      if (cfg_.debug) {
        std::cout << "Cannot handle unknown bytecode: returning" << std::endl;
      }
      handled = false;
      break;
  }

  return handled;
}

/*************************************************
 * GENERATE CODE FOR BYTECODES
 *************************************************/

void MethodBuilder::handle_bc_jmp(
    TR::BytecodeBuilder *builder,
    const std::vector<TR::BytecodeBuilder *> &bytecodeBuilderTable,
    const std::vector<Instruction> &program, long bytecodeIndex,
    TR::BytecodeBuilder *nextBuilder) {
  Instruction instruction = program[bytecodeIndex];
  int delta = instruction.immediate() + 1;
  int next_bc_index = bytecodeIndex + delta;
  TR::BytecodeBuilder *destBuilder = bytecodeBuilderTable[next_bc_index];
  builder->Goto(destBuilder);
}

void MethodBuilder::handle_bc_jmp_eq(
    TR::BytecodeBuilder *builder,
    const std::vector<TR::BytecodeBuilder *> &bytecodeBuilderTable,
    const std::vector<Instruction> &program, long bytecodeIndex,
    TR::BytecodeBuilder *nextBuilder) {
  Instruction instruction = program[bytecodeIndex];
  int delta = instruction.immediate() + 1;
  int next_bc_index = bytecodeIndex + delta;
  TR::BytecodeBuilder *jumpTo = bytecodeBuilderTable[next_bc_index];

  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  builder->IfCmpEqual(jumpTo, left, right);
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_jmp_neq(
    TR::BytecodeBuilder *builder,
    const std::vector<TR::BytecodeBuilder *> &bytecodeBuilderTable,
    const std::vector<Instruction> &program, long bytecodeIndex,
    TR::BytecodeBuilder *nextBuilder) {
  Instruction instruction = program[bytecodeIndex];
  int delta = instruction.immediate() + 1;
  int next_bc_index = bytecodeIndex + delta;
  TR::BytecodeBuilder *jumpTo = bytecodeBuilderTable[next_bc_index];

  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  builder->IfCmpNotEqual(jumpTo, left, right);
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_jmp_lt(
    TR::BytecodeBuilder *builder,
    const std::vector<TR::BytecodeBuilder *> &bytecodeBuilderTable,
    const std::vector<Instruction> &program, long bytecodeIndex,
    TR::BytecodeBuilder *nextBuilder) {
  Instruction instruction = program[bytecodeIndex];
  int delta = instruction.immediate() + 1;
  int next_bc_index = bytecodeIndex + delta;
  TR::BytecodeBuilder *jumpTo = bytecodeBuilderTable[next_bc_index];

  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  builder->IfCmpLessThan(jumpTo, left, right);
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_jmp_le(
    TR::BytecodeBuilder *builder,
    const std::vector<TR::BytecodeBuilder *> &bytecodeBuilderTable,
    const std::vector<Instruction> &program, long bytecodeIndex,
    TR::BytecodeBuilder *nextBuilder) {
  Instruction instruction = program[bytecodeIndex];
  int delta = instruction.immediate() + 1;
  int next_bc_index = bytecodeIndex + delta;
  TR::BytecodeBuilder *jumpTo = bytecodeBuilderTable[next_bc_index];

  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  builder->IfCmpLessOrEqual(jumpTo, left, right);
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_jmp_gt(
    TR::BytecodeBuilder *builder,
    const std::vector<TR::BytecodeBuilder *> &bytecodeBuilderTable,
    const std::vector<Instruction> &program, long bytecodeIndex,
    TR::BytecodeBuilder *nextBuilder) {
  Instruction instruction = program[bytecodeIndex];
  int delta = instruction.immediate() + 1;
  int next_bc_index = bytecodeIndex + delta;
  TR::BytecodeBuilder *jumpTo = bytecodeBuilderTable[next_bc_index];

  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  builder->IfCmpGreaterThan(jumpTo, left, right);
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_jmp_ge(
    TR::BytecodeBuilder *builder,
    const std::vector<TR::BytecodeBuilder *> &bytecodeBuilderTable,
    const std::vector<Instruction> &program, long bytecodeIndex,
    TR::BytecodeBuilder *nextBuilder) {
  Instruction instruction = program[bytecodeIndex];
  int delta = instruction.immediate() + 1;
  int next_bc_index = bytecodeIndex + delta;
  TR::BytecodeBuilder *jumpTo = bytecodeBuilderTable[next_bc_index];

  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  builder->IfCmpGreaterOrEqual(jumpTo, left, right);
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_sub(TR::BytecodeBuilder *builder,
                                  TR::BytecodeBuilder *nextBuilder) {
  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  pushInt48(builder, builder->Sub(left, right));
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_add(TR::BytecodeBuilder *builder,
                                  TR::BytecodeBuilder *nextBuilder) {
  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  pushInt48(builder, builder->Add(left, right));
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_mul(TR::BytecodeBuilder *builder,
                                  TR::BytecodeBuilder *nextBuilder) {
  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  pushInt48(builder, builder->Mul(left, right));
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_div(TR::BytecodeBuilder *builder,
                                  TR::BytecodeBuilder *nextBuilder) {
  TR::IlValue *right = popInt48(builder);
  TR::IlValue *left = popInt48(builder);

  pushInt48(builder, builder->Div(left, right));
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::handle_bc_not(TR::BytecodeBuilder *builder,
                                  TR::BytecodeBuilder *nextBuilder) {
  auto zero = builder->ConstInteger(globalTypes().stackElement, 0);
  auto value = popInt48(builder);
  auto result = builder->ConvertTo(globalTypes().stackElement,
                                   builder->EqualTo(value, zero));
  pushInt48(builder, result);
  builder->AddFallThroughBuilder(nextBuilder);
}

void MethodBuilder::drop(TR::BytecodeBuilder *builder) { popValue(builder); }

/// Input is a Value.
void MethodBuilder::pushValue(TR::BytecodeBuilder *builder,
                              TR::IlValue *value) {
  if (cfg_.lazyVmState) {
    VirtualMachineState *vmState =
        dynamic_cast<VirtualMachineState *>(builder->vmState());

    vmState->_stack->Push(builder, value);

  } else {
    TR::IlValue *stack = builder->StructFieldInstanceAddress(
        "b9::ExecutionContext", "stack_", builder->Load("executionContext"));

    TR::IlValue *stackTop =
        builder->LoadIndirect("b9::OperandStack", "top_", stack);

    builder->StoreAt(stackTop,
                     builder->ConvertTo(globalTypes().stackElement, value));

    TR::IlValue *newStackTop = builder->IndexAt(
        globalTypes().stackElementPtr, stackTop, builder->ConstInt32(1));

    builder->StoreIndirect("b9::OperandStack", "top_", stack, newStackTop);
  }
}

/// output is an unboxed value.
TR::IlValue *MethodBuilder::popValue(TR::BytecodeBuilder *builder) {
  if (cfg_.lazyVmState) {
    VirtualMachineState *vmState =
        dynamic_cast<VirtualMachineState *>(builder->vmState());

    TR::IlValue *result = vmState->_stack->Pop(builder);

    return result;

  } else {
    TR::IlValue *stack = builder->StructFieldInstanceAddress(
        "b9::ExecutionContext", "stack_", builder->Load("executionContext"));

    TR::IlValue *stackTop =
        builder->LoadIndirect("b9::OperandStack", "top_", stack);

    TR::IlValue *newStackTop = builder->IndexAt(
        globalTypes().stackElementPtr, stackTop, builder->ConstInt32(-1));

    builder->StoreIndirect("b9::OperandStack", "top_", stack, newStackTop);

    TR::IlValue *value =
        builder->LoadAt(globalTypes().stackElementPtr, newStackTop);

    return value;
  }
}

/// input is an unboxed int48.
void MethodBuilder::pushInt48(TR::BytecodeBuilder *builder,
                              TR::IlValue *value) {
  pushValue(builder, ValueBuilder::fromInt48(builder, value));
}

TR::IlValue *MethodBuilder::popInt48(TR::BytecodeBuilder *builder) {
  return ValueBuilder::getInt48(builder, popValue(builder));
}

}  // namespace b9
