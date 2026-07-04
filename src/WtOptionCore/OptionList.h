/*!
 * \file OptionList.h
 * \brief Template multi-index container for options (migrated from quantbox)
 *
 * Original: longbeach::optioncore::OptionList used boost::multi_index_container
 * keyed on instrument_t / expiry_t / right_t. The migrated version keeps the
 * same boost::multi_index infrastructure but uses wt_option types:
 *  - instrument_t -> std::string (stdCode)
 *  - expiry_t     -> uint32_t (YYYYMM)
 *  - right_t      -> wt_option::OptionRight
 *  - strike_t     -> double
 *  - boost::shared_ptr -> std::shared_ptr
 *
 * The element type T must expose:
 *   std::string        getInstrument() const
 *   const expiry_t&    getExpiry() const      (expiry_t = uint32_t)
 *   double             getStrikePrice() const
 *   OptionRight        getRight() const
 */
#ifndef WTOPTIONCORE_OPTIONLIST_H_INCLUDED
#define WTOPTIONCORE_OPTIONLIST_H_INCLUDED

#define BOOST_MULTI_INDEX_DISABLE_SERIALIZATION
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/composite_key.hpp>
#undef BOOST_MULTI_INDEX_DISABLE_SERIALIZATION

#include "optioncoretypes.h"

#include <memory>
#include <string>

namespace wt_option {

// expiry_t in wt_option is uint32_t (YYYYMM). We need a reference-returning
// accessor for multi_index, so element types should return by value (uint32_t).
// The original returned const expiry_t& — with uint32_t that's impractical, so
// we switch to by-value const_mem_fun for the expiry key.

template <typename T>
class OptionList : public boost::multi_index::multi_index_container<
    std::shared_ptr<T>,
    boost::multi_index::indexed_by<
        // 0: ordered-unique by instrument code (std::string)
        boost::multi_index::ordered_unique<
            boost::multi_index::const_mem_fun<T, std::string, &T::getInstrument>
        >,
        // 1: ordered-non-unique by expiry (uint32_t YYYYMM)
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<uint32_t>,
            boost::multi_index::const_mem_fun<T, uint32_t, &T::getExpiry>
        >,
        // 2: hashed-unique by (expiry, strike, right) composite
        boost::multi_index::hashed_unique<
            boost::multi_index::composite_key<
                std::shared_ptr<T>,
                boost::multi_index::const_mem_fun<T, uint32_t, &T::getExpiry>,
                boost::multi_index::const_mem_fun<T, double, &T::getStrikePrice>,
                boost::multi_index::const_mem_fun<T, OptionRight, &T::getRight>
            >
        >
    >
>
{
};

} // namespace wt_option

#endif // WTOPTIONCORE_OPTIONLIST_H_INCLUDED
