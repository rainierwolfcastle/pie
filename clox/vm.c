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

inline void push(Value value) {
    *vm.stack_top = value;
    vm.stack_top++;
}

inline Value pop(void) {
    vm.stack_top--;
    return *vm.stack_top;
}

inline static Value peek(int distance) {
    return vm.stack_top[-1 - distance];
}

inline static bool call(ObjClosure *closure, int arg_count) {
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

inline static bool call_value(Value callee, int arg_count) {
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

inline static bool invoke_from_class(ObjClass *klass, ObjString *name, int arg_count) {
    Value method;
    if (!table_get(&klass->methods, name, &method)) {
        runtime_error("Undefined property '%s'.", name->chars);
        return false;
    }
    return call(AS_CLOSURE(method), arg_count);
}

inline static bool invoke(ObjString *name, int arg_count) {
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

inline static bool bind_method(ObjClass *klass, ObjString *name) {
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

inline static ObjUpvalue* capture_upvalue(Value *local) {
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

inline static void close_upvalues(Value *last) {
    while (vm.open_upvalues != NULL && vm.open_upvalues->location >= last) {
        ObjUpvalue *upvalue = vm.open_upvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.open_upvalues = upvalue->next;
    }
}

inline static void define_method(ObjString *name) {
    Value method = peek(0);
    ObjClass *klass = AS_CLASS(peek(1));
    table_set(&klass->methods, name, method);
    pop();
}

inline static bool is_falsey(Value v) {
    return IS_NIL(v) || (IS_BOOL(v) && !AS_BOOL(v));
}

static inline void concatinate(void) {
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

    static void *opcodes[] = {
        &&OP_CONSTANT,
        &&OP_NIL,
        &&OP_TRUE,
        &&OP_FALSE,
        &&OP_POP,
        &&OP_GET_LOCAL,
        &&OP_SET_LOCAL,
        &&OP_GET_GLOBAL,
        &&OP_DEFINE_GLOBAL,
        &&OP_SET_GLOBAL,
        &&OP_GET_UPVALUE,
        &&OP_SET_UPVALUE,
        &&OP_GET_PROPERTY,
        &&OP_SET_PROPERTY,
        &&OP_GET_SUPER,
        &&OP_EQUAL,
        &&OP_GREATER,
        &&OP_LESS,
        &&OP_ADD,
        &&OP_SUBTRACT,
        &&OP_MULTIPLY,
        &&OP_DIVIDE,
        &&OP_NOT,
        &&OP_NEGATE,
        &&OP_PRINT,
        &&OP_JUMP,
        &&OP_JUMP_IF_FALSE,
        &&OP_LOOP,
        &&OP_CALL,
        &&OP_INVOKE,
        &&OP_SUPER_INVOKE,
        &&OP_CLOSURE,
        &&OP_CLOSE_UPVALUE,
        &&OP_RETURN,
        &&OP_CLASS,
        &&OP_INHERIT,
        &&OP_METHOD,
    };

#ifdef DEBUG_TRACE_EXECUTION
#define NEXT() goto DEBUG_PRINT
#else
#define NEXT() goto *opcodes[(*ip++)]
#endif
#define READ_BYTE() (*ip++)
#define READ_SHORT() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(value_type, op) \
    do { \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
            frame->ip = ip; \
            runtime_error("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        double b = AS_NUMBER(pop()); \
        double a = AS_NUMBER(pop()); \
        push(value_type(a op b)); \
    } while (false)

    NEXT();

#ifdef DEBUG_TRACE_EXECUTION
    DEBUG_PRINT:
        printf("          ");
        for (Value *slot = vm.stack; slot < vm.stack_top; slot++) {
            printf("[ ");
            print_value(*slot);
            printf(" ]");
        }
        printf("\n");
        disassemble_instruction(&frame->closure->function->chunk, (int)(ip - frame->closure->function->chunk.code));

        goto *opcodes[(*ip++)];
#endif
    OP_CONSTANT:
        {
            Value constant = READ_CONSTANT();
            push(constant);
            NEXT();
        }
    OP_NIL:
        push(NIL_VAL);
        NEXT();
    OP_TRUE:
        push(BOOL_VAL(true));
        NEXT();
    OP_FALSE:
        push(BOOL_VAL(false));
        NEXT();
    OP_POP:
        pop();
        NEXT();
    OP_GET_LOCAL:
        {
            uint8_t slot = READ_BYTE();
            push(frame->slots[slot]);
            NEXT();
        }
    OP_SET_LOCAL:
        {
            uint8_t slot = READ_BYTE();
            frame->slots[slot] = peek(0);
            NEXT();
        }
    OP_GET_GLOBAL:
        {
            ObjString *name = READ_STRING();
            Value value;
            if (!table_get(&vm.globals, name, &value)) {
                frame->ip = ip;
                runtime_error("Undefined variable '%s'.", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            push(value);
            NEXT();
        }
    OP_DEFINE_GLOBAL:
        {
            ObjString *name = READ_STRING();
            table_set(&vm.globals, name, peek(0));
            pop();
            NEXT();
        }
    OP_SET_GLOBAL:
        {
            ObjString *name = READ_STRING();
            if (table_set(&vm.globals, name, peek(0))) {
                table_delete(&vm.globals, name);
                frame->ip = ip;
                runtime_error("Undefined variable '%s'.", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            NEXT();
        }
    OP_GET_UPVALUE:
        {
            uint8_t slot = READ_BYTE();
            push(*frame->closure->upvalues[slot]->location);
            NEXT();
        }
    OP_SET_UPVALUE:
        {
            uint8_t slot = READ_BYTE();
            *frame->closure->upvalues[slot]->location = peek(0);
            NEXT();
        }
    OP_GET_PROPERTY:
        {
            if (!IS_INSTANCE(peek(0))) {
                frame->ip = ip;
                runtime_error("Only instances have properties.");
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjInstance *instance = AS_INSTANCE(peek(0));
            ObjString *name = READ_STRING();

            Value value;
            if (table_get(&instance->fields, name, &value)) {
                pop();
                push(value);
                NEXT();
            }

            if (!bind_method(instance->klass, name)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            NEXT();
        }
    OP_SET_PROPERTY:
        {
            if (!IS_INSTANCE(peek(1))) {
                frame->ip = ip;
                runtime_error("Only instances have fields.");
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjInstance *instance = AS_INSTANCE(peek(1));
            table_set(&instance->fields, READ_STRING(), peek(0));
            Value value = pop();
            pop();
            push(value);
            NEXT();
        }
    OP_GET_SUPER:
        {
            ObjString *name = READ_STRING();
            ObjClass *superclass = AS_CLASS(pop());

            if (!bind_method(superclass, name)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            NEXT();
        }
    OP_EQUAL:
        {
            Value b = pop();
            Value a = pop();
            push(BOOL_VAL(values_equal(a, b)));
            NEXT();
        }
    OP_GREATER:
        BINARY_OP(BOOL_VAL, >);
        NEXT();
    OP_LESS:
        BINARY_OP(BOOL_VAL, <);
        NEXT();
    OP_ADD:
        {
            if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
                concatinate();
            } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                double b = AS_NUMBER(pop());
                double a = AS_NUMBER(pop());
                push(NUMBER_VAL(a + b));
            } else {
                frame->ip = ip;
                runtime_error("Operands must be two numbers of two strings.");
                return INTERPRET_RUNTIME_ERROR;
            }
            NEXT();
        }
    OP_SUBTRACT:
        BINARY_OP(NUMBER_VAL, -);
        NEXT();
    OP_MULTIPLY:
        BINARY_OP(NUMBER_VAL, *);
        NEXT();
    OP_DIVIDE:
        BINARY_OP(NUMBER_VAL, /);
        NEXT();
    OP_NOT:
        push(BOOL_VAL(is_falsey(pop())));
        NEXT();
    OP_NEGATE:
        {
            if (!IS_NUMBER(peek(0))) {
                frame->ip = ip;
                runtime_error("Operand must be a number.");
                return INTERPRET_RUNTIME_ERROR;
            }
            push(NUMBER_VAL(-AS_NUMBER(pop())));
            NEXT();
        }
    OP_PRINT:
        {
            print_value(pop());
            printf("\n");
            NEXT();
        }
    OP_JUMP:
        {
            uint16_t offset = READ_SHORT();
            ip += offset;
            NEXT();
        }
    OP_JUMP_IF_FALSE:
        {
            uint16_t offset = READ_SHORT();
            if (is_falsey(peek(0))) ip += offset;
            NEXT();
        }
    OP_LOOP:
        {
            uint16_t offset = READ_SHORT();
            ip -= offset;
            NEXT();
        }
    OP_CALL:
        {
            int arg_count = READ_BYTE();
            frame->ip = ip;
            if (!call_value(peek(arg_count), arg_count)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            ip = frame->ip;
            NEXT();
        }
    OP_INVOKE:
        {
            ObjString *method = READ_STRING();
            int arg_count = READ_BYTE();
            if (!invoke(method, arg_count)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            NEXT();
        }
    OP_SUPER_INVOKE:
        {
            ObjString *method = READ_STRING();
            int arg_count = READ_BYTE();
            ObjClass *superclass = AS_CLASS(pop());
            if (!invoke_from_class(superclass, method, arg_count)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            NEXT();
        }
    OP_CLOSURE:
        {
            ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
            ObjClosure *closure = new_closure(function);
            push(OBJ_VAL(closure));

            for (int i = 0; i < closure->upvalue_count; i++) {
                uint8_t is_local = READ_BYTE();
                uint8_t index = READ_BYTE();
                if (is_local) {
                    closure->upvalues[i] = capture_upvalue(frame->slots + index);
                } else {
                    closure->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            NEXT();
        }
    OP_CLOSE_UPVALUE:
        {
            close_upvalues(vm.stack_top - 1);
            pop();
            NEXT();
        }
    OP_RETURN:
        {
            Value result = pop();
            close_upvalues(frame->slots);
            vm.frame_count--;
            if (vm.frame_count == 0) {
                pop();
                return INTERPRET_OK;
            }

            vm.stack_top = frame->slots;
            push(result);
            frame = &vm.frames[vm.frame_count - 1];
            ip = frame->ip;
            NEXT();
        }
    OP_CLASS:
        push(OBJ_VAL(new_class(READ_STRING())));
        NEXT();
    OP_INHERIT:
        {
            Value superclass = peek(1);
            if (!IS_CLASS((superclass))) {
                frame->ip = ip;
                runtime_error("Superclass must be a class.");
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjClass *subclass = AS_CLASS(peek(0));
            table_add_all(&AS_CLASS(superclass)->methods, &subclass->methods);
            pop();
            NEXT();
        }
    OP_METHOD:
        define_method(READ_STRING());
        NEXT();

#undef NEXT
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
