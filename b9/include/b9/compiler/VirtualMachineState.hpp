#if !defined(B9_VIRTUALMACHINESTATE_HPP_)
#define B9_VIRTUALMACHINESTATE_HPP_

#include <ilgen/VirtualMachineOperandStack.hpp>
#include <ilgen/VirtualMachineRegister.hpp>
#include <ilgen/VirtualMachineRegisterInStruct.hpp>
#include <ilgen/VirtualMachineState.hpp>

namespace b9 {

/// Simulates all state of the virtual machine state while compiled code is
/// running. It simulates the stack and the pointer to the top of the stack.
class VirtualMachineState : public TR::VirtualMachineState {
 public:
  VirtualMachineState() = default;

  VirtualMachineState(TR::VirtualMachineOperandStack *stack,
                      TR::VirtualMachineRegister *stackTop)
      : _stack(stack), _stackTop(stackTop) {}

  void Commit(TR::IlBuilder *b) override {
    _stack->Commit(b);
    _stackTop->Commit(b);
  }

  void Reload(TR::IlBuilder *b) override {
    _stackTop->Reload(b);
    _stack->Reload(b);
  }

  VirtualMachineState *MakeCopy() override {
    auto newState = new VirtualMachineState();
    newState->_stack =
        dynamic_cast<TR::VirtualMachineOperandStack *>(_stack->MakeCopy());
    newState->_stackTop =
        dynamic_cast<TR::VirtualMachineRegister *>(_stackTop->MakeCopy());
    return newState;
  }

  void MergeInto(TR::VirtualMachineState *other, TR::IlBuilder *b) override {
    auto otherState = dynamic_cast<VirtualMachineState *>(other);
    _stack->MergeInto(otherState->_stack, b);
    _stackTop->MergeInto(otherState->_stackTop, b);
  }

  TR::VirtualMachineOperandStack *_stack = nullptr;
  TR::VirtualMachineRegister *_stackTop = nullptr;
};

}  // namespace b9

#endif  // B9_VIRTUALMACHINESTATE_HPP_
