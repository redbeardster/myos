// stddef.h
#ifndef STDDEF_H
#define STDDEF_H

typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef unsigned int wchar_t;

#define NULL ((void*)0)

// Смещение поля в структуре
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
