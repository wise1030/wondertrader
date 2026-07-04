/*!
 * \file UftOptionStraFact.cpp
 * \brief UFT Strategy Factory for Option Market-Making
 *
 * Exports createStrategyFact/deleteStrategyFact for WtUftRunner dynamic library loading.
 * Pattern follows WtFutuCore/UftFutuMmStrategy.cpp:2828-2866.
 */
#include "UftOptionStrategy.h"
#include "../Includes/UftStrategyDefs.h"

#include <cstring>

class OptionStraFact : public IUftStrategyFact
{
public:
    virtual const char* getName() override { return "OptionStraFact"; }

    virtual void enumStrategy(FuncEnumUftStrategyCallback cb) override
    {
        cb(getName(), "OptionMM", true);
    }

    virtual UftStrategy* createStrategy(const char* name, const char* id) override
    {
        if (strcmp(name, "OptionMM") == 0)
            return new UftOptionStrategy(id);
        return nullptr;
    }

    virtual bool deleteStrategy(UftStrategy* stra) override
    {
        delete stra;
        return true;
    }
};

extern "C" {
    EXPORT_FLAG IUftStrategyFact* createStrategyFact()
    {
        return new OptionStraFact();
    }

    EXPORT_FLAG void deleteStrategyFact(IUftStrategyFact*& fact)
    {
        if (fact)
        {
            delete fact;
            fact = nullptr;
        }
    }
}
