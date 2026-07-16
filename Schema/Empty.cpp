// The build tool needs at least one source file per library to produce a compile step. This
// file includes both generated wire headers so they are compiled on every build — which is
// what makes their compile-time size checks (each struct's size must equal its declared
// on-wire length) run every time, catching a bad schema regeneration immediately.
#include "ILink3Sbe.hpp"
#include "Mdp3Sbe.hpp"
