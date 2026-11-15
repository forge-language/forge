# Ryu binary64 formatter

Source: https://github.com/ulfjack/ryu/tree/4c0618b0e44f7ef027ebae05d2cc7812048f7c8f
Revision: 4c0618b0e44f7ef027ebae05d2cc7812048f7c8f.
Files under ryu/ retain upstream code; trailing blank lines are normalized.
License: Apache-2.0 OR BSL-1.0; both license texts are included alongside them.

cmake/FloatFormat.cmake generates SDK headers with local includes, omits the
unused public allocating APIs, and gives the buffered formatter internal inline
linkage. compiler/float_format.h prefixes internal symbols and adapts shortest
digits to Forge's accepted D1 notation. No network fetch occurs during builds.
