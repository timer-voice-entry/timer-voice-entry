#pragma once

#include <stdexcept>

namespace tt
{

    struct Error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // Thrown when an id refers to nothing.
    struct NotFound : Error
    {
        using Error::Error;
    };

    struct Conflict : Error
    {
        using Error::Error;
    };

    struct Invalid : Error
    {
        using Error::Error;
    };

}
