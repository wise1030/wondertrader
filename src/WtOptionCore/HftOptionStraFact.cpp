/*!
 * \file HftOptionStraFact.cpp
 * \brief HFT Strategy Factory for Option Market-Making
 *
 * Exports createStrategyFact/deleteStrategyFact for HftStrategyMgr dynamic library loading.
 */
#include "HftOptionStrategy.h"
#include "../Includes/HftStrategyDefs.h"

#include <cstring>

class OptionStraFact : public IHftStrategyFact
{
public:
    virtual const char* getName() override { return "OptionStraFact"; }

    virtual void enumStrategy(FuncEnumHftStrategyCallback cb) override
    {
        cb(getName(), "OptionMM", true);
    }

    virtual HftStrategy* createStrategy(const char* name, const char* id) override
    {
        if (strcmp(name, "OptionMM") == 0)
            return new HftOptionStrategy(id);
        return nullptr;
    }

    virtual bool deleteStrategy(HftStrategy* stra) override
    {
        delete stra;
        return true;
    }
};

extern "C" {
    EXPORT_FLAG IHftStrategyFact* createStrategyFact()
    {
        return new OptionStraFact();
    }

    EXPORT_FLAG void deleteStrategyFact(IHftStrategyFact*& fact)
    {
        if (fact)
        {
            delete fact;
            fact = nullptr;
        }
    }
}
