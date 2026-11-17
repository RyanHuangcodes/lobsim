#pragma once

namespace sim {

// How a noise trader measures its limit-order offset from a two-sided local BBO.
//   Floor:     buy at floor(mid) - d, sell at floor(mid) + d (the rule of SPEC 5.6).
//   Symmetric: buy at ceil(mid) - d,  sell at floor(mid) + d. With an odd spread the mid
//              is x.5 and Floor lets sells improve the ask while buys can only join the
//              bid, which makes the price drift down; Symmetric removes that bias.
enum class ReferencePrice { Floor, Symmetric };

}  // namespace sim
