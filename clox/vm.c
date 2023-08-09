#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "compiler.h"
#include "common.h"
#include "debug.h"
#include "memory.h"
#include "vm.h"

Vm vm;

static Value clock_native(int arg_count, Value *args) {
    return NUMBER_VAL((double) clock() / CLOCKS_PER_SEC);
}

static void reset_stack(void) {
    vm.stack_top = vm.stack;
    vm.frame_count = 0;
    vm.open_upvalues = NULL;
}

static void runtime_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);
    
    for (int i = vm.frame_count - 1; i >= 0; i--) {
        CallFrame *frame = &vm.frames[i];
        ObjFunction *function = frame->closure->function;
        size_t instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }
    
    reset_stack();
}

static void define_native(const char *name, NativeFn function) {
    push(OBJ_VAL(copy_string(name, (int) strlen(name))));
    push(OBJ_VAL(new_native(function)));
    table_set(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
    pop();
    pop();
}

void init_vm(void) {
    reset_stack();
    vm.objects = NULL;
    vm.bytes_allocated = 0;
    vm.next_gc = 1024 * 1024;
    
    vm.gray_count = 0;
    vm.gray_capacity = 0;
    vm.gray_stack = NULL;
    
    init_table(&vm.globals);
    init_table(&vm.strings);
    
    vm.init_string = NULL;
    vm.init_string = copy_string("init", 4);
    
    define_native("clock", clock_native);
}

void free_vm(void) {
    free_table(&vm.globals);
    free_table(&vm.strings);
    vm.init_string = NULL;
    free_objects();
}

void push(Value value) {
    *vm.stack_top = value;
    vm.stack_top++;
}

Value pop(void) {
    vm.stack_top--;
    return *vm.stack_top;
}

static Value peek(int distance) {
    return vm.stack_top[-1 - distance];
}

static bool call(ObjClosure *closure, int arg_count) {
    if (arg_count != closure->function->arity) {
        runtime_error("Expected %d arguments but got %d.", closure->function->arity, arg_count);
        return false;
    }
    
    if (vm.frame_count == FRAMES_MAX) {
        runtime_error("Stack overflow.");
        return false;
    }
    
    CallFrame *frame = &vm.frames[vm.frame_count++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = vm.stack_top - arg_count - 1;
    return true;
}

static bool call_value(Value callee, int arg_count) {
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
                vm.stack_top[-arg_count - 1] = bound->receiver;
                return call(bound->method, arg_count);
            }
            case OBJ_CLASS: {
                ObjClass *klass = AS_CLASS(callee);
                vm.stack_top[-arg_count - 1] = OBJ_VAL(new_instance(klass));
                Value initializer;
                if (table_get(&klass->methods, vm.init_string, &initializer)) {
                    return call(AS_CLOSURE(initializer), arg_count);
                } else if (arg_count != 0) {
                    runtime_error("Expected 0 arguments but got %d.", arg_count);
                    return false;
                }
                return true;
            }
            case OBJ_CLOSURE:
                return call(AS_CLOSURE(callee), arg_count);
            case OBJ_NATIVE: {
                NativeFn native = AS_NATIVE(callee);
                Value result = native(arg_count, vm.stack_top - arg_count);
                vm.stack_top -= arg_count + 1;
                push(result);
                return true;
            }
            default:
                break;
        }
    }
    
    runtime_error("Can only call functions and classes.");
    return false;
}

static bool invoke_from_class(ObjClass *klass, ObjString *name, int arg_count) {
    Value method;
    if (!table_get(&klass->methods, name, &method)) {
        runtime_error("Undefined property '%s'.", name->chars);
        return false;
    }
    return call(AS_CLOSURE(method), arg_count);
}

static bool invoke(ObjString *name, int arg_count) {
    Value receiver = peek(arg_count);
    
    if (!IS_INSTANCE(receiver)) {
        runtime_error("Only instances have methods.");
        return false;
    }
    
    ObjInstance *instance = AS_INSTANCE(receiver);
    
    Value value;
    if (table_get(&instance->fields, name, &value)) {
        vm.stack_top[-arg_count - 1] = value;
        return call_value(value, arg_count);
    }
    
    return invoke_from_class(instance->klass, name, arg_count);
}

static bool bind_method(ObjClass *klass, ObjString *name) {
    Value method;
    if (!table_get(&klass->methods, name, &method)) {
        runtime_error("Undefined property '%s'.", name->chars);
        return false;
    }
    
    ObjBoundMethod *bound = new_bound_method(peek(0), AS_CLOSURE(method));
    pop();
    push(OBJ_VAL(bound));
    return true;
}

static ObjUpvalue* capture_upvalue(Value *local) {
    ObjUpvalue *prev_upvalue = NULL;
    ObjUpvalue *upvalue = vm.open_upvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prev_upvalue = upvalue;
        upvalue = upvalue->next;
    }
    
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }
    
    ObjUpvalue *created_upvalue = new_upvalue(local);
    created_upvalue->next = upvalue;
    
    if (prev_upvalue == NULL) {
        vm.open_upvalues = created_upvalue;
    } else {
        prev_upvalue->next = created_upvalue;
    }
    
    return created_upvalue;
}

static void close_upvalues(Value *last) {
    while (vm.open_upvalues != NULL && vm.open_upvalues->location >= last) {
        ObjUpvalue *upvalue = vm.open_upvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.open_upvalues = upvalue->next;
    }
}

static void define_method(ObjString *name) {
    Value method = peek(0);
    ObjClass *klass = AS_CLASS(peek(1));
    table_set(&klass->methods, name, method);
    pop();
}

static bool is_falsey(Value v) {
    return IS_NIL(v) || (IS_BOOL(v) && !AS_BOOL(v));
}

static void concatinate(void) {
    ObjString *b = AS_STRING(peek(0));
    ObjString *a = AS_STRING(peek(1));
    
    int length = a->length + b->length;
    char *chars = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';
    
    ObjString *result = take_string(chars, length);
    pop();
    pop();
    push(OBJ_VAL(result));
}

static InterpretResult run(void) {
    register CallFrame *frame = &vm.frames[vm.frame_count - 1];
    register uint8_t *ip = frame->ip;

#define PUSH(value) (*vm.stack_top++ = value)
#define POP() (*(--vm.stack_top))
#define PEEK(distance) (vm.stack_top[-1 - distance])
#define STORE_FRAME() (frame->ip = ip)
#define LOAD_FRAME() (ip = frame->ip)
#define READ_BYTE() (*ip++)
#define READ_SHORT() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(value_type, op) \
    do { \
        if (!IS_NUMBER(PEEK(0)) || !IS_NUMBER(PEEK(1))) { \
            STORE_FRAME(); \
            runtime_error("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        double b = AS_NUMBER(POP()); \
        double a = AS_NUMBER(POP()); \
        PUSH(value_type(a op b)); \
    } while (false)

    for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
    printf("          ");
    for (Value *slot = vm.stack; slot < vm.stack_top; slot++) {
      printf("[ ");
      print_value(*slot);
      printf(" ]");
    }
    printf("\n");
    disassemble_instruction(&frame->closure->function->chunk, (int)(ip - frame->closure->function->chunk.code));
#endif
        uint8_t instruction;
        switch (instruction = READ_BYTE()) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                PUSH(constant);
                break;
            }
            case OP_NIL: PUSH(NIL_VAL); break;
            case OP_TRUE: PUSH(BOOL_VAL(true)); break;
            case OP_FALSE: PUSH(BOOL_VAL(false)); break;
            case OP_POP: POP(); break;
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                PUSH(frame->slots[slot]);
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = PEEK(0);
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString *name = READ_STRING();
                Value value;
                if (!table_get(&vm.globals, name, &value)) {
                    STORE_FRAME();
                    runtime_error("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                PUSH(value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString *name = READ_STRING();
                table_set(&vm.globals, name, PEEK(0));
                POP();
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString *name = READ_STRING();
                if (table_set(&vm.globals, name, PEEK(0))) {
                    table_delete(&vm.globals, name);
                    STORE_FRAME();
                    runtime_error("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                PUSH(*frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = PEEK(0);
                break;
            }
            case OP_GET_PROPERTY: {
                if (!IS_INSTANCE(PEEK(0))) {
                    STORE_FRAME();
                    runtime_error("Only instances have properties.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                ObjInstance *instance = AS_INSTANCE(PEEK(0));
                ObjString *name = READ_STRING();
                
                Value value;
                if (table_get(&instance->fields, name, &value)) {
                    POP();
                    PUSH(value);
                    break;
                }
                
                if (!bind_method(instance->klass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SET_PROPERTY: {
                if (!IS_INSTANCE(PEEK(1))) {
                    STORE_FRAME();
                    runtime_error("Only instances have fields.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                ObjInstance *instance = AS_INSTANCE(PEEK(1));
                table_set(&instance->fields, READ_STRING(), PEEK(0));
                Value value = POP();
                POP();
                PUSH(value);
                break;
            }
            case OP_GET_SUPER: {
                ObjString *name = READ_STRING();
                ObjClass *superclass = AS_CLASS(POP());
                
                if (!bind_method(superclass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_EQUAL: {
                Value b = POP();
                Value a = POP();
                PUSH(BOOL_VAL(values_equal(a, b)));
                break;
            }
            case OP_GREATER:  BINARY_OP(BOOL_VAL, >); break;
            case OP_LESS:     BINARY_OP(BOOL_VAL, <); break;
            case OP_ADD:      {
                if (IS_STRING(PEEK(0)) && IS_STRING(PEEK(1))) {
                    concatinate();
                } else if (IS_NUMBER(PEEK(0)) && IS_NUMBER(PEEK(1))) {
                    double b = AS_NUMBER(POP());
                    double a = AS_NUMBER(POP());
                    PUSH(NUMBER_VAL(a + b));
                } else {
                    STORE_FRAME();
                    runtime_error("Operands must be two numbers of two strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
            case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
            case OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); break;
            case OP_NOT: PUSH(BOOL_VAL(is_falsey(POP()))); break;
            case OP_NEGATE:
                if (!IS_NUMBER(PEEK(0))) {
                    STORE_FRAME();
                    runtime_error("Operand must be a number.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                PUSH(NUMBER_VAL(-AS_NUMBER(POP())));
                break;
            case OP_PRINT: {
                print_value(POP());
                printf("\n");
                break;
            }
            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (is_falsey(PEEK(0))) ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                ip -= offset;
                break;
            }
            case OP_CALL: {
                int arg_count = READ_BYTE();
                STORE_FRAME();
                if (!call_value(PEEK(arg_count), arg_count)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frame_count - 1];
                LOAD_FRAME();
                break;
            }
            case OP_INVOKE: {
                ObjString *method = READ_STRING();
                int arg_count = READ_BYTE();
                if (!invoke(method, arg_count)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }
            case OP_SUPER_INVOKE: {
                ObjString *method = READ_STRING();
                int arg_count = READ_BYTE();
                ObjClass *superclass = AS_CLASS(POP());
                if (!invoke_from_class(superclass, method, arg_count)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }
            case OP_CLOSURE: {
                ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure *closure = new_closure(function);
                PUSH(OBJ_VAL(closure));
                
                for (int i = 0; i < closure->upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (is_local) {
                        closure->upvalues[i] = capture_upvalue(frame->slots + index);
                    } else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                break;
            }
            case OP_CLOSE_UPVALUE:
                close_upvalues(vm.stack_top - 1);
                POP();
                break;
            case OP_RETURN: {
                Value result = POP();
                close_upvalues(frame->slots);
                vm.frame_count--;
                if (vm.frame_count == 0) {
                    POP();
                    return INTERPRET_OK;
                }
                
                vm.stack_top = frame->slots;
                PUSH(result);
                frame = &vm.frames[vm.frame_count - 1];
                LOAD_FRAME();
                break;
            }
            case OP_CLASS:
                PUSH(OBJ_VAL(new_class(READ_STRING())));
                break;
            case OP_INHERIT: {
                Value superclass = PEEK(1);
                if (!IS_CLASS((superclass))) {
                    STORE_FRAME();
                    runtime_error("Superclass must be a class.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                ObjClass *subclass = AS_CLASS(PEEK(0));
                table_add_all(&AS_CLASS(superclass)->methods, &subclass->methods);
                POP();
                break;
            }
            case OP_METHOD:
                define_method(READ_STRING());
                break;
        }
    }

#undef PUSH
#undef POP
#undef PEEK
#undef STORE_FRAME
#undef LOAD_FRAME
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

InterpretResult interpret(const char *source) {
    ObjFunction *function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;
    
    push(OBJ_VAL(function));
    ObjClosure *closure = new_closure(function);
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);
    
    return run();
}
