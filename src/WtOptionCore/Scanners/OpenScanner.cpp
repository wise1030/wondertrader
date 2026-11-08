#include "OpenScanner.h"

namespace wt_option {

// R6: factory registration — closes the "factory map empty" defect
REGISTER_SCANNER("OpenScanner", OpenScanner);

} // namespace wt_option
