#ifndef XtcData_Src_hh
#define XtcData_Src_hh

#include "xtcdata/xtc/Level.hh"
#include <stdint.h>

namespace XtcData
{

class Src
{
public:
    Src();
    Src(uint32_t value) : _value(value) {}
    Src(Level::Type level);

    uint32_t log() const;
    uint32_t phy() const;
    uint32_t nodeId() const {return _value;}

    Level::Type level() const;

    bool operator==(const Src& s) const;
    bool operator<(const Src& s) const;

    static uint32_t _sizeof()
    {
        return sizeof(Src);
    }

    void phy(uint32_t value) { _value = value; }

  protected:
    uint32_t _log;   // cpo: eliminate this when we change xtc format
    uint32_t _value;
};
}
#endif
