#pragma once
#include <cstdint>

namespace pocketpp {

enum class Opcode : uint8_t {
    PUSH_CONSTANT,    // 2 bytes: uint16 index into constants pool
    PUSH_NULL,
    PUSH_0,
    PUSH_TRUE,
    PUSH_FALSE,
    POP,
    DUP,
    SWAP,
    PUSH_LOCAL_0, PUSH_LOCAL_1, PUSH_LOCAL_2, PUSH_LOCAL_3, PUSH_LOCAL_4,
    PUSH_LOCAL_5, PUSH_LOCAL_6, PUSH_LOCAL_7, PUSH_LOCAL_8,
    PUSH_LOCAL_N,     // 1 byte: slot index
    STORE_LOCAL_0, STORE_LOCAL_1, STORE_LOCAL_2, STORE_LOCAL_3, STORE_LOCAL_4,
    STORE_LOCAL_5, STORE_LOCAL_6, STORE_LOCAL_7, STORE_LOCAL_8,
    STORE_LOCAL_N,    // 1 byte: slot index (leaves value on stack)
    PUSH_GLOBAL,      // 2 bytes: name constant index
    STORE_GLOBAL,     // 2 bytes: name constant index (leaves value on stack)
    PUSH_UPVALUE,     // 1 byte: upvalue index
    STORE_UPVALUE,    // 1 byte: upvalue index (leaves value on stack)
    PUSH_SELF,
    PUSH_LIST,        // 2 bytes: element count
    PUSH_MAP,
    LIST_APPEND,
    MAP_INSERT,
    PUSH_CLOSURE,     // 2 bytes: FuncChunk constant index; followed by [is_local:1, idx:1]*N
    CREATE_CLASS,     // 2 bytes: name constant index; parent on stack
    BIND_METHOD,      // 2 bytes: method name constant index
    CLOSE_UPVALUE,
    IMPORT,           // 2 bytes: path constant index
    CALL,             // 1 byte: argc
    TAIL_CALL,        // 1 byte: argc
    METHOD_CALL,      // 2 bytes: name idx, 1 byte: argc
    SUPER_CALL,       // 2 bytes: name idx, 1 byte: argc
    RETURN,
    ITER_TEST,
    ITER,             // 2 bytes: jump offset if done
    YIELD,
    JUMP,             // 2 bytes: forward offset
    LOOP,             // 2 bytes: backward offset
    JUMP_IF,          // 2 bytes: pop, jump if truthy
    JUMP_IF_NOT,      // 2 bytes: pop, jump if falsy
    OR,               // 2 bytes: if TOS truthy don't pop+jump, else pop
    AND,              // 2 bytes: if TOS falsy don't pop+jump, else pop
    GET_ATTRIB,       // 2 bytes: name idx
    GET_ATTRIB_KEEP,  // 2 bytes: name idx; keeps obj on stack
    SET_ATTRIB,       // 2 bytes: name idx
    GET_SUBSCRIPT,
    GET_SUBSCRIPT_KEEP,
    SET_SUBSCRIPT,
    POSITIVE, NEGATIVE, NOT, BIT_NOT,
    ADD, SUBTRACT, MULTIPLY, DIVIDE, EXPONENT, MOD,
    BIT_AND, BIT_OR, BIT_XOR, BIT_LSHIFT, BIT_RSHIFT,
    EQEQ, NOTEQ, LT, LTEQ, GT, GTEQ,
    RANGE, IN, NOT_IN, IS,
    STRING_CONCAT,    // 1 byte: count N
    EXTEND_INPLACE,   // like ADD but mutates list LHS in-place (for +=)
    END,
};

} // namespace pocketpp
