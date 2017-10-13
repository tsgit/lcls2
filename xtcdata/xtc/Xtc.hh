#ifndef XtcData_Xtc_hh
#define XtcData_Xtc_hh

#include "Damage.hh"
#include "Src.hh"
#include "xtcdata/xtc/TypeId.hh"

#include <stddef.h>
#include <stdint.h>

namespace XtcData
{

class Xtc
{
public:
    Xtc() : damage(0), extent(0){};
    Xtc(const Xtc& xtc)
    : damage(xtc.damage), src(xtc.src), contains(xtc.contains), extent(sizeof(Xtc))
    {
    }
    Xtc(const TypeId& type) : damage(0), contains(type), extent(sizeof(Xtc))
    {
    }
    Xtc(const TypeId& type, const Src& _src)
    : damage(0), src(_src), contains(type), extent(sizeof(Xtc))
    {
    }
    Xtc(const TypeId& _tag, const Src& _src, unsigned _damage)
    : damage(_damage), src(_src), contains(_tag), extent(sizeof(Xtc))
    {
    }
    Xtc(const TypeId& _tag, const Src& _src, const Damage& _damage)
    : damage(_damage), src(_src), contains(_tag), extent(sizeof(Xtc))
    {
    }

    void* operator new(size_t size, char* p)
    {
        return (void*)p;
    }
    void* operator new(size_t size, Xtc* p)
    {
        return p->alloc(size);
    }
    void* operator new(size_t size, Xtc& p)
    {
        return p.alloc(size);
    }

    char* payload() const
    {
        return (char*)(this + 1);
    }
    int sizeofPayload() const
    {
        return extent - sizeof(Xtc);
    }
    Xtc* next()
    {
        return (Xtc*)((char*)this + extent);
    }
    const Xtc* next() const
    {
        return (const Xtc*)((char*)this + extent);
    }

    void* alloc(uint32_t size)
    {
        void* buffer = next();
        extent += size;
        return buffer;
    }

    Damage damage;
    Src src;
    TypeId contains;
    uint32_t extent;
};
}


#endif
