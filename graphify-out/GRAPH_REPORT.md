# Graph Report - lobsim  (2026-11-18)

## Corpus Check
- 83 files · ~72,957 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 2 file(s) not represented in the graph (top: (none) 1, .cmake 1)

## Summary
- 2536 nodes · 5686 edges · 137 communities (122 shown, 15 thin omitted)
- Extraction: 88% EXTRACTED · 12% INFERRED · 0% AMBIGUOUS · INFERRED: 678 edges (avg confidence: 0.89)
- Token cost: 9,000 input · 3,500 output

## Community Hubs (Navigation)
- Core Types and Harnesses
- Order Log and Replay
- ReferenceBook Implementation
- Metrics Observer Core
- OrderBook Public API
- Market Maker Stress Probe
- Metrics Scripted Tests
- Avellaneda-Stoikov Market Maker
- Seeded Differential Fuzz Generator
- Book Harness and Conservation
- Order Request Value Types
- CMake Build Targets
- Market Maker Unit Tests
- Summary Statistics Output
- Run Counts and Fills
- Engine Spec and Unit Tests
- Simulator Components Spec
- Markout Decomposition
- Agent Context and Traders
- OrderBook Matching Internals
- Noise Trader
- Simulator Run Helpers
- Simulation Config Structures
- Determinism Tests
- Microbenchmark Book Models
- OrderBook Validate Tests
- Observer and Event Timing
- Noise Trader Tests
- Parameter Sweep Script
- Matching Semantics Decisions
- Event Queue Ordering
- Order Enums and Takers
- Market Maker Test Harness
- Cancel and Modify Tests
- Build Spec and Invariants
- Object Pool Internals
- Simulator Core Config
- Informed Trader
- OrderBook Levels and Links
- Exchange Wrapper
- A-S Quote Formulas
- Book Views and Snapshots
- Fuzz Coverage Counters
- Matching Tests
- Spread Statistics
- README Engine Overview
- Config Validation Tests
- Informed Trader Tests
- Noise Reference Price
- Market Maker Stress Results
- MM Pending Requests
- Config JSON Parser
- Simulation Kernel Tests
- Xoshiro RNG Distributions
- Integration Tests
- Metrics Options and Roles
- Bench Message Streams
- Plotting Script
- Self-Trade Prevention Tests
- Latency Channels
- Spread Histogram Plot
- Timer Queue
- RNG Tests
- Reject Reasons
- Kernel Test Probes
- Bench Pass Results
- Event Hash Smoke Tests
- FOK and Post-Only Tests
- RNG Stream Seeding
- Event Hasher Encoding
- BBO Market Data
- Run Result Files
- Market Maker Spec
- Market Data Callbacks
- Validation Tests
- Microbenchmarks
- Trade Records
- Agent Context Scheduling
- README Simulator Overview
- Fake Test Context
- Aggressor Test Agent
- Run Command Line
- Return Statistics
- BBO and Spread Capture Rows
- MM Total Stats
- Bench RNG
- MM Window Stats
- README Analysis Tools
- Market Data Probes
- Query API Tests
- SplitMix64 Test Generator
- Agent Interface
- Simulator Construction Options
- MM PnL Chart
- Return ACF Chart
- Reference Resting Orders
- MM Working Orders
- Noise Trader Params
- Queue Test Context
- Allocation Counting Hooks
- Allocation Totals
- Bench Hash Pass
- Exchange Tests
- Microbenchmark Decisions
- Request Handling
- README Order Types
- README Project Overview
- README Market Maker
- README Run and Replay
- Simulator Agent Slots
- Modify Request Semantics
- Cancelled Event
- Ack Event
- Exchange Output
- Private Order Observer
- Object Pool Tests
- Modified Event
- Fundamental Value Process
- SplitMix Seeding
- README Determinism
- Noise Trader Spec
- Latency Percentiles
- Order Id Index
- Mid vs Fundamental Chart
- Simulation Event Kinds
- Pool Test Items
- Allocation Audit Rows
- Bench Due Orders
- Run Metrics Printing
- Book Config
- Trade Prints
- MM Quote Orders
- RNG Spec
- Cancel Request
- Fuzz Regimes
- MM Test New Info

## God Nodes (most connected - your core abstractions)
1. `OrderBook` - 109 edges
2. `MetricsObserver` - 92 edges
3. `TimeNs` - 89 edges
4. `Price` - 84 edges
5. `OrderId` - 75 edges
6. `Qty` - 72 edges
7. `AvellanedaStoikovMM` - 71 edges
8. `ReferenceBook` - 64 edges
9. `MmProbe` - 58 edges
10. `Side` - 57 edges

## Surprising Connections (you probably didn't know these)
- `Spread Histogram (seed 42, after warm-up)` --semantically_similar_to--> `histogram`  [INFERRED] [semantically similar]
  docs/sample_run/spread_hist.png → include/sim/metrics.hpp
- `Stylized Facts (heavy tails, no return autocorrelation, volatility clustering)` --semantically_similar_to--> `Stylized Facts of Asset Returns`  [INFERRED] [semantically similar]
  SPEC.md → docs/sample_run/return_acf.png
- `Market-maker inventory, sampled (seed 42)` --shares_data_with--> `mm_inventory`  [INFERRED]
  docs/sample_run/mm_inventory.png → include/sim/metrics.hpp
- `Xoshiro256StarStar` --implements--> `xoshiro256** Generator`  [INFERRED]
  include/sim/rng.hpp → SPEC.md
- `Accelerating market-maker PnL growth` --conceptually_related_to--> `reservation_price()`  [AMBIGUOUS]
  docs/sample_run/mm_pnl.png → src/sim/as_market_maker.cpp

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Replay verification flow (orders.log to fresh book to identical event hash)** — readme_exchange, readme_order_log_codec, readme_orders_log, readme_lobsim_replay, readme_orderbook, readme_engine_event_hash, readme_summary_json [EXTRACTED 1.00]
- **Determinism mechanisms of the simulator** — readme_determinism, readme_custom_distributions, readme_per_agent_rng_streams, readme_seq_tie_break, readme_ffp_contract_off, readme_observers [INFERRED 0.85]
- **Differential testing of the fast book against the reference book** — readme_orderbook, readme_referencebook, readme_two_books_one_behaviour, readme_fuzz_diff, readme_lob_tests, readme_deep_book_test, readme_engine_event_hash [EXTRACTED 1.00]
- **bbo.csv to mid-vs-fundamental plot pipeline** — src_sim_metrics_metricsobserver_write_bbo_csv, tools_plot_main, docs_sample_run_mid_vs_fundamental [INFERRED 0.95]
- **Price discovery: informed trading pulls the mid toward the latent fundamental** — include_sim_fundamental_fundamental, include_sim_agents_informed_trader_informedtrader, docs_sample_run_mid_vs_fundamental_exchange_mid, docs_sample_run_mid_vs_fundamental_mid_fundamental_gap [INFERRED 0.85]
- **MM inventory chart pipeline: sample, bbo.csv, plot.py, mm_inventory.png** — src_sim_metrics_metricsobserver_take_sample, include_sim_metrics_metricsobserver_bborow_mm_inventory, src_sim_metrics_metricsobserver_write_bbo_csv, tools_plot_main, docs_sample_run_mm_inventory [INFERRED 0.95]
- **Market-maker PnL from metrics observer to chart** — src_sim_metrics_metricsobserver_pnl_now, src_sim_metrics_metricsobserver_take_sample, include_sim_metrics_metricsobserver_bborow_mm_pnl, src_sim_metrics_metricsobserver_write_bbo_csv, tools_plot_main, docs_sample_run_mm_pnl [INFERRED 0.95]
- **Warm-up window across config, metrics and plots** — include_sim_config_simconfig_warmup_s, include_sim_metrics_metricsoptions_warmup_ns, include_sim_metrics_mmwindowstats_pnl_after_warmup, tools_plot_shade_warmup, docs_sample_run_mm_pnl_warm_up_window [INFERRED 0.85]
- **Return ACF diagnostic of the seed 42 run** — docs_sample_run_return_acf_seed_42_run, docs_sample_run_return_acf_warm_up_period, docs_sample_run_return_acf_acf_of_returns, docs_sample_run_return_acf_acf_of_absolute_returns, docs_sample_run_return_acf_significance_band [EXTRACTED 1.00]
- **Sampled vs Time-Weighted Spread Distribution** — docs_sample_run_spread_hist_spread_histogram, docs_sample_run_spread_hist_bid_ask_spread, include_sim_metrics_metricsobserver_bbo_rows, include_sim_metrics_metricsobserver_spread_time, include_sim_metrics_spreadstats_histogram [INFERRED 0.85]
- **Design rules derived from lessons of existing simulators and exchanges (Section 2)** — spec_integer_ticks_and_lots, spec_price_level_map_with_intrusive_fifo_lists, spec_discrete_event_simulation, spec_explicit_sequence_tie_breaking, spec_fills_only_through_matching_engine, spec_interactive_agent_based_simulation, spec_no_assumed_fills, spec_modify_semantics, spec_self_trade_prevention, spec_latency_distribution_reporting [EXTRACTED 1.00]
- **Adverse-selection experiment: fundamental value feeds the informed trader, whose flow hits the A–S market maker, measured by markouts** — spec_fundamental_value, spec_informed_trader, spec_avellanedastoikovmm, spec_markouts, spec_adverse_selection, spec_no_assumed_fills [EXTRACTED 1.00]
- **Correctness and determinism verification: oracle book, differential fuzzing, invariants and event hashes** — spec_referencebook, spec_orderbook, spec_differential_fuzz_test, spec_book_invariants, spec_event_hash, spec_determinism_test, spec_replay_test, spec_sanitizer_build [INFERRED 0.85]
- **Determinism and reproducibility rules** — decisions_d_015, decisions_d_047, decisions_d_070, decisions_d_071, decisions_d_072, decisions_d_078, decisions_d_097 [INFERRED 0.85]
- **Markout decomposition: markout = spread_capture + mid_move** — decisions_d_106, decisions_d_112, decisions_d_113, decisions_d_114, decisions_d_115 [EXTRACTED 1.00]
- **Run, log, replay and hash-check pipeline** — cmakelists_cli_run_then_replay, cmakelists_lobsim_run, cmakelists_lobsim_replay, src_sim_replay_replay_log, decisions_orders_log, decisions_summary_json, decisions_d_074, decisions_d_110 [EXTRACTED 1.00]

## Communities (137 total, 15 thin omitted)

### Community 0 - "Core Types and Harnesses"
Cohesion: 0.06
Nodes (27): allocation_audit(), compiler(), cpu_model(), main(), mean_ns(), measure_timer(), msgs_per_sec(), now_ns() (+19 more)

### Community 1 - "Order Log and Replay"
Cohesion: 0.05
Nodes (27): ReplayError, ReplayResult, event_hash, events, max_price, requests, trades, Exchange::Exchange() (+19 more)

### Community 2 - "ReferenceBook Implementation"
Cohesion: 0.06
Nodes (31): ReferenceBook, cfg_, MatchResult, filled, stopped_by_stp, next_order_id_, next_priority_seq_, next_trade_id_ (+23 more)

### Community 3 - "Metrics Observer Core"
Cohesion: 0.05
Nodes (38): MetricsObserver, cash_, counts_, fees_, fills_, final_inventory_value_, final_pnl_, fundamental_ (+30 more)

### Community 4 - "OrderBook Public API"
Cohesion: 0.07
Nodes (20): EventBuffer, OrderBook, asks_, bids_, cfg_, index_, MatchResult, filled (+12 more)

### Community 5 - "Market Maker Stress Probe"
Cohesion: 0.07
Nodes (18): MmProbe, index_, max_abs_q_, messages_, mm_, new_orders_, order_side_, owner_ (+10 more)

### Community 6 - "Metrics Scripted Tests"
Cohesion: 0.06
Nodes (16): ledger(), lim(), lines(), mkt(), run_script(), Script, Scripted, bbo_csv (+8 more)

### Community 7 - "Avellaneda-Stoikov Market Maker"
Cohesion: 0.07
Nodes (19): AvellanedaStoikovMM, bbo_, end_, next_client_id_, p_, q_, requote_interval_, sides_ (+11 more)

### Community 8 - "Seeded Differential Fuzz Generator"
Cohesion: 0.11
Nodes (12): OrderId, Price, compare_order(), compare_queries(), differ(), Generator, center_, pos_ (+4 more)

### Community 9 - "Book Harness and Conservation"
Cohesion: 0.08
Nodes (11): BookHarness, cfg_, checker_, highest_id_, Events, ConservationChecker, open_, remaining_ (+3 more)

### Community 10 - "Order Request Value Types"
Cohesion: 0.10
Nodes (35): Event, NewOrder, client_order_id, owner, post_only, price, qty, side (+27 more)

### Community 11 - "CMake Build Targets"
Cohesion: 0.10
Nodes (26): cli_run_then_replay command-line test, -ffp-contract=off compile option (no fused multiply-add), fuzz_diff differential fuzz target, Google Benchmark v1.8.3 dependency, GoogleTest v1.14.0 dependency, lob static library target, lobsim_bench throughput benchmark target, lobsim_options warnings interface target (+18 more)

### Community 13 - "Summary Statistics Output"
Cohesion: 0.13
Nodes (26): Interval, hi, lo, Series, Weighted, append_ll(), autocorrelation(), confidence_interval_95() (+18 more)

### Community 14 - "Run Counts and Fills"
Cohesion: 0.06
Nodes (28): maker_fee_ticks, taker_fee_ticks, Fill, in_window, inventory_after, maker, markouts, price (+20 more)

### Community 15 - "Engine Spec and Unit Tests"
Cohesion: 0.13
Nodes (20): lob_tests GoogleTest executable, Cboe Futures Exchange BOEv3 Modify Order Specification, Coinbase Exchange Matching Engine (self-trade prevention), Core Types (Price, Qty, OrderId, OwnerId, Side, OrderType, TIF, STP), Differential Fuzz Test (OrderBook vs ReferenceBook), Engine Events (Ack, Reject, Trade, Cancelled, Modified), Limit Order Book (LOB), lob Library (matching engine) (+12 more)

### Community 16 - "Simulator Components Spec"
Cohesion: 0.09
Nodes (26): Byrd, Hybinette, Balch: ABIDES, Towards High-Fidelity Market Simulation for AI Research, Agent Interface (on_start, on_wakeup, on_market_data, on_private), Bit-Exact on Every Public Scenario and About 90x Faster (NeurIPS 2026), Coletta et al., Towards Realistic Market Simulations: a GAN Approach, Cont (2001) Stylized Facts, via Ratliff-Crain et al., Revisiting Cont's Stylized Facts for Modern Stock Markets, Event Queue (min-heap on (time_ns, seq)), Exact First, Then Fast: An Event-for-Event C++ Rebuild of the Agenthon ABIDES Market (NeurIPS 2026), Exchange (wraps one OrderBook) (+18 more)

### Community 17 - "Markout Decomposition"
Cohesion: 0.09
Nodes (23): Metrics, output files, replay and plots (Phase 7; SPEC 5.7, 6, 9 item 7), mm_fills.csv market-maker fills output, Decomposition, markout_mean, mid_move_ci_hi, mid_move_ci_lo, mid_move_mean, mid_move_mean_qty_weighted (+15 more)

### Community 18 - "Agent Context and Traders"
Cohesion: 0.18
Nodes (11): Context, seconds_to_ns(), AvellanedaStoikovMM::on_start(), AvellanedaStoikovMM::on_wakeup(), AvellanedaStoikovMM::requote_side(), InformedTrader::on_start(), InformedTrader::on_wakeup(), NoiseTrader::on_private() (+3 more)

### Community 19 - "OrderBook Matching Internals"
Cohesion: 0.10
Nodes (16): fail(), OrderBook::best_ask(), OrderBook::best_bid(), OrderBook::best_level(), OrderBook::depth(), OrderBook::fok_fillable(), OrderBook::fok_fillable_in(), OrderBook::match() (+8 more)

### Community 20 - "Noise Trader"
Cohesion: 0.08
Nodes (13): NoiseTrader, awaiting_ack_, bbo_, last_trade_, limits_sent_, markets_sent_, next_client_id_, open_ (+5 more)

### Community 21 - "Simulator Run Helpers"
Cohesion: 0.08
Nodes (12): Simulator, agents_, cfg_, fundamental_k_, market_maker_, observers_, processed_, queue_ (+4 more)

### Community 22 - "Simulation Config Structures"
Cohesion: 0.08
Nodes (22): FundamentalConfig, dt_s, sigma_ticks_per_sqrt_s, SimConfig, duration_ns, duration_s, fundamental, fundamental_dt_ns (+14 more)

### Community 23 - "Determinism Tests"
Cohesion: 0.09
Nodes (7): default_for(), expect_common_prefix_equal(), expect_noise_decisions_unchanged(), projection(), Row, TEST(), with_jitter()

### Community 24 - "Microbenchmark Book Models"
Cohesion: 0.10
Nodes (13): Book, cursor, ev, levels, next_id, order, Resting, id (+5 more)

### Community 25 - "OrderBook Validate Tests"
Cohesion: 0.09
Nodes (4): expect_invalid(), healthy(), OrderBookTestPeer, TEST()

### Community 26 - "Observer and Event Timing"
Cohesion: 0.11
Nodes (13): TimeNs, Observer, Simulator::add_observer(), Counter, books, end_time, ends, engine_events (+5 more)

### Community 27 - "Noise Trader Tests"
Cohesion: 0.09
Nodes (8): new_order(), params(), priced(), run_mode(), Sent, orders, rng, TEST()

### Community 28 - "Parameter Sweep Script"
Cohesion: 0.16
Nodes (14): final_mid(), informed_variants(), main(), mean(), noise_only(), mutate(), read_csv(), Runner (+6 more)

### Community 29 - "Matching Semantics Decisions"
Cohesion: 0.13
Nodes (13): Avellaneda-Stoikov market maker (Phase 6; SPEC 1, 5.6 item 3, 9 item 6), Matching engine API (Section 3 is silent on exact signatures), Matching semantics (cases Section 3 leaves open), CancelReason, FOKUnfillable, IOCRemainder, NoLiquidity, SelfTrade (+5 more)

### Community 30 - "Event Queue Ordering"
Cohesion: 0.11
Nodes (11): EventQueue, heap_, next_seq_, Payload, SimEvent, agent, kind, payload (+3 more)

### Community 31 - "Order Enums and Takers"
Cohesion: 0.08
Nodes (25): Taker, id, is_market, limit, owner, remaining, side, stp (+17 more)

### Community 32 - "Market Maker Test Harness"
Cohesion: 0.15
Nodes (15): ack(), book(), buy_up_to(), cid(), expect_new(), fill(), Harness, ctx (+7 more)

### Community 33 - "Cancel and Modify Tests"
Cohesion: 0.09
Nodes (3): ioc(), partially_filled_front(), TYPED_TEST()

### Community 34 - "Build Spec and Invariants"
Cohesion: 0.16
Nodes (15): print_percentiles(), Array-ladder price index (SPEC Section 10 stretch goal), lobsim Build Specification (SPEC.md), Brave New Geek, Benchmarking Message Queue Latency, Coordinated Omission, lobsim_bench Throughput Harness, Order Node (intrusive doubly linked FIFO entry), OrderBook (fast book) (+7 more)

### Community 35 - "Object Pool Internals"
Cohesion: 0.13
Nodes (5): OrderBook internals and the differential fuzz test (Phase 3; Section 3.5 is silent on these), ObjectPool, free_, Slot, bytes

### Community 36 - "Simulator Core Config"
Cohesion: 0.09
Nodes (16): Simulator core (Phase 5), InformedConfig, lambda, latency, size, theta_ticks, MarketMakerConfig, gamma (+8 more)

### Community 37 - "Informed Trader"
Cohesion: 0.11
Nodes (12): InformedTrader, bbo_, next_client_id_, p_, InformedTraderParams, lambda, size, theta_ticks (+4 more)

### Community 38 - "OrderBook Levels and Links"
Cohesion: 0.10
Nodes (20): Level, count, head, price, tail, total_qty, Order, id (+12 more)

### Community 39 - "Exchange Wrapper"
Cohesion: 0.13
Nodes (7): Exchange, hasher_, line_, log_, out_, Other, orders_sent

### Community 40 - "A-S Quote Formulas"
Cohesion: 0.14
Nodes (15): MmParams, gamma, kappa, max_price, q_max, quote_size, sigma, Quotes (+7 more)

### Community 41 - "Book Views and Snapshots"
Cohesion: 0.10
Nodes (15): LevelView, price, total_qty, OrderView, id, owner, post_only, price (+7 more)

### Community 42 - "Fuzz Coverage Counters"
Cohesion: 0.10
Nodes (17): Coverage, book_emptied, cancels, events, fok_filled, fok_unfillable, max_resting, messages (+9 more)

### Community 43 - "Matching Tests"
Cohesion: 0.11
Nodes (3): MatchingTest, TYPED_TEST(), worked_example_asks()

### Community 44 - "Spread Statistics"
Cohesion: 0.12
Nodes (17): MetricsSummary, counts, has_mm, markouts, mm_total, mm_window, returns, spread (+9 more)

### Community 45 - "README Engine Overview"
Cohesion: 0.18
Nodes (17): Coordinated omission, Deep-book test, Differential fuzz test (fuzz_diff), Google Benchmark v1.8.3, googletest v1.14.0, Heap allocation audit, Intrusive doubly linked FIFO per price level, lob_tests unit tests (+9 more)

### Community 47 - "Informed Trader Tests"
Cohesion: 0.12
Nodes (4): fixed(), order(), TEST(), wake_with()

### Community 48 - "Noise Reference Price"
Cohesion: 0.13
Nodes (13): NoiseTradersConfig, count, D, lambda, latency, max_size, order_lifetime_mean_s, p_limit (+5 more)

### Community 49 - "Market Maker Stress Results"
Cohesion: 0.11
Nodes (16): StressResult, anomalies, buy_fills, cancel_raced_by_full_fill, cancels, checked_at_end, fill_while_other_side_pending, max_abs_q (+8 more)

### Community 50 - "MM Pending Requests"
Cohesion: 0.13
Nodes (15): Pending, client_order_id, kind, order_id, price, qty, PendingKind, Cancel (+7 more)

### Community 51 - "Config JSON Parser"
Cohesion: 0.35
Nodes (13): at(), bad(), check_keys(), check_latency(), integer(), latency(), latency_json(), load_config() (+5 more)

### Community 53 - "Xoshiro RNG Distributions"
Cohesion: 0.19
Nodes (5): Xoshiro256StarStar, has_spare_, s_, spare_, Own Distributions (uniform int, uniform [0,1), exponential, Box–Muller normal)

### Community 54 - "Integration Tests"
Cohesion: 0.14
Nodes (7): finalize(), positive(), drift(), TEST(), default_file_config(), stress(), TEST()

### Community 55 - "Metrics Options and Roles"
Cohesion: 0.12
Nodes (17): MetricsOptions, end_ns, initial_fundamental, maker_fee_ticks, markout_horizons_ns, markout_horizons_s, mm_owner, roles (+9 more)

### Community 56 - "Bench Message Streams"
Cohesion: 0.13
Nodes (10): percentiles(), Stream, cancels_, kLifetimeMeanNewOrders, kLimitShare, kMaxPrice, kOwners, new_orders_ (+2 more)

### Community 57 - "Plotting Script"
Cohesion: 0.23
Nodes (6): Market-maker inventory, sampled (seed 42), autocorrelation(), empty_note(), fail(), main(), shade_warmup()

### Community 58 - "Self-Trade Prevention Tests"
Cohesion: 0.12
Nodes (3): stp_asks(), StpTest, TYPED_TEST()

### Community 59 - "Latency Channels"
Cohesion: 0.13
Nodes (6): Channel, base_, jitter_, last_arrival_, latency_rng, TEST()

### Community 60 - "Spread Histogram Plot"
Cohesion: 0.14
Nodes (13): bbo.csv sampled top-of-book output, Warm-up window (shaded), Seed-42 sample run, Bid-Ask Spread, Sample Times, Seed 42 Sample Run, Spread Histogram (seed 42, after warm-up), Spread Mostly 2 Ticks (~85%), 1 Tick ~10%, 3 Ticks ~4% (+5 more)

### Community 61 - "Timer Queue"
Cohesion: 0.15
Nodes (9): CancelTimer, order_id, TimerQueue, Entry, order, payload, time, heap_ (+1 more)

### Community 63 - "Reject Reasons"
Cohesion: 0.13
Nodes (16): RejectReason, InvalidFlags, InvalidPrice, InvalidQty, NoChange, NotOwner, RequestKind, Cancel (+8 more)

### Community 64 - "Kernel Test Probes"
Cohesion: 0.16
Nodes (9): ChainProbe, first_, id_, later_, TimerProbe, id_, times_, Trace (+1 more)

### Community 65 - "Bench Pass Results"
Cohesion: 0.15
Nodes (13): PassResult, allocs, ask_levels, bid_levels, engine_ns, event_counts, events, hash (+5 more)

### Community 66 - "Event Hash Smoke Tests"
Cohesion: 0.13
Nodes (7): Reject, client_order_id, order_id, owner, reason, request_kind, TEST()

### Community 67 - "FOK and Post-Only Tests"
Cohesion: 0.14
Nodes (3): FokTest, PostOnlyTest, TYPED_TEST()

### Community 68 - "RNG Stream Seeding"
Cohesion: 0.23
Nodes (11): make_stream(), stream_id(), stream_seed(), StreamPurpose, Decisions, Latency, StreamRole, InformedTrader (+3 more)

### Community 69 - "Event Hasher Encoding"
Cohesion: 0.23
Nodes (4): EventHasher, h_, kOffset, kPrime

### Community 70 - "BBO Market Data"
Cohesion: 0.17
Nodes (9): BBO, ask, ask_qty, bid, bid_qty, Exchange::process(), top_of_book(), MetricsObserver::on_book() (+1 more)

### Community 71 - "Run Result Files"
Cohesion: 0.13
Nodes (13): RunFiles, bbo_csv, hash, mm_fills_csv, orders_log, summary_json, trades_csv, RunResult (+5 more)

### Community 72 - "Market Maker Spec"
Cohesion: 0.22
Nodes (12): markout_horizons_s, Adverse Selection, Avellaneda & Stoikov (2008), via Hummingbot's Technical Deep Dive, Avellaneda–Stoikov (A–S) Model, AvellanedaStoikovMM Agent, Inventory Risk, Lalor & Swishchuk, Market Simulation under Adverse Selection, Market Making (+4 more)

### Community 73 - "Market Data Callbacks"
Cohesion: 0.15
Nodes (8): MarketData, NoiseTrader::on_market_data(), ScriptAgent, events, market, next_, script_, sent_at

### Community 75 - "Microbenchmarks"
Cohesion: 0.31
Nodes (10): BM_AddExistingLevel(), BM_AddNewLevel(), BM_BestPrices(), BM_CalibrationEmptyBatchWithoutPause(), BM_CalibrationEmptyBatchWithPause(), BM_Cancel(), BM_MatchOneLevel(), BM_ModifyReduce() (+2 more)

### Community 76 - "Trade Records"
Cohesion: 0.15
Nodes (12): Trade, maker_order_id, maker_owner, price, qty, taker_order_id, taker_owner, trade_id (+4 more)

### Community 77 - "Agent Context Scheduling"
Cohesion: 0.19
Nodes (3): AgentIndex, Simulator::AgentContext, index_

### Community 78 - "README Simulator Overview"
Cohesion: 0.28
Nodes (12): EventQueue, Exchange, Fundamental value V, InformedTrader, Latency channels (entry, response, feed), Market order, NoiseTrader, Order-log codec (+4 more)

### Community 79 - "Fake Test Context"
Cohesion: 0.15
Nodes (5): FakeCtx, pending, r, sent, t

### Community 80 - "Aggressor Test Agent"
Cohesion: 0.17
Nodes (8): Aggressor, bbo_, lean_, next_cid_, open_, rate_, ref_, stop_at_

### Community 81 - "Run Command Line"
Cohesion: 0.17
Nodes (5): main(), parse_double(), parse_u64(), usage(), write_file()

### Community 82 - "Return Statistics"
Cohesion: 0.20
Nodes (11): Sample, ReturnStats, abs_acf, acf, excess_kurtosis, mean, missing, returns (+3 more)

### Community 83 - "BBO and Spread Capture Rows"
Cohesion: 0.17
Nodes (11): BboRow, mm_inventory, time, top, SpreadCaptureStats, ci_hi, ci_lo, mean (+3 more)

### Community 84 - "MM Total Stats"
Cohesion: 0.17
Nodes (12): MmTotalStats, buy_fills, cash, fees, fills, final_inventory, inventory_value, max_abs_inventory (+4 more)

### Community 86 - "MM Window Stats"
Cohesion: 0.18
Nodes (9): MmWindowStats, avg_quoted_spread, buy_fills, fills, max_abs_inventory, quoting_fraction, sell_fills, volume_buy (+1 more)

### Community 87 - "README Analysis Tools"
Cohesion: 0.18
Nodes (10): Adverse selection, bbo.csv, Bid-ask bounce, Informed-trader variant sweep, Markouts (spread capture + mid move), MetricsObserver, mm_fills.csv, Stylized facts analysis (+2 more)

### Community 88 - "Market Data Probes"
Cohesion: 0.20
Nodes (7): MdProbe, id_, next_, script_, MdTrace, items, Script

### Community 91 - "Agent Interface"
Cohesion: 0.22
Nodes (4): Agent, lifetime_rng_, Lifetimes, Simulator::agent()

### Community 92 - "Simulator Construction Options"
Cohesion: 0.20
Nodes (8): Test, Simulator::Simulator(), Options, enforce_coverage, first_seed, full_scan_every, messages, seeds

### Community 93 - "MM PnL Chart"
Cohesion: 0.24
Nodes (9): Market-maker PnL, sampled (seed 42), Accelerating market-maker PnL growth, Mark-to-market PnL (ticks), Warm-up window (0-60 s), mm_pnl, pnl_at_warmup_, warmup_ns, pnl_after_warmup (+1 more)

### Community 94 - "Return ACF Chart"
Cohesion: 0.33
Nodes (6): Return Autocorrelation Plot (seed 42, after warm-up), ACF of Absolute Returns (n = 3540, lags 1-20), ACF of Returns (n = 3540, lags 1-20), Negative Lag-1 Return Autocorrelation (about -0.35), No Significant Return Autocorrelation Beyond Lag 2, Seed 42 Run

### Community 95 - "Reference Resting Orders"
Cohesion: 0.20
Nodes (9): Resting, id, owner, post_only, price, priority_seq, remaining, side (+1 more)

### Community 96 - "MM Working Orders"
Cohesion: 0.24
Nodes (5): WorkingOrder, id, price, remaining, Working

### Community 97 - "Noise Trader Params"
Cohesion: 0.20
Nodes (10): NoiseTraderParams, D, initial_price, lambda, max_price, max_size, order_lifetime_mean_s, p_limit (+2 more)

### Community 98 - "Queue Test Context"
Cohesion: 0.20
Nodes (4): ScriptCtx, r, t, wakeups

### Community 99 - "Allocation Counting Hooks"
Cohesion: 0.39
Nodes (6): allocate(), allocate_aligned(), operator delete(), operator new(), record(), release()

### Community 100 - "Allocation Totals"
Cohesion: 0.22
Nodes (9): AllocTotals, allocs, by_size, bytes, frees, other_sizes, SizeCount, count (+1 more)

### Community 101 - "Bench Hash Pass"
Cohesion: 0.28
Nodes (3): run_pass(), hash_bytes(), hash_event()

### Community 103 - "Microbenchmark Decisions"
Cohesion: 0.33
Nodes (3): micro_bench Google Benchmark target, Benchmarks (Phase 4), Microbenchmarks (Google Benchmark, 1k and 100k-order books)

### Community 104 - "Request Handling"
Cohesion: 0.22
Nodes (5): Request, with_owner(), Simulator::handle_request(), expect_cancel(), expect_modify()

### Community 105 - "README Order Types"
Cohesion: 0.28
Nodes (5): Limit FOK order, Limit GTC order, Limit IOC order, lob (matching engine library), Post-only order

### Community 106 - "README Project Overview"
Cohesion: 0.31
Nodes (7): Array-indexed price ladder (stretch goal), DECISIONS.md (decision log), lobsim, LOBSTER data replay (stretch goal), Modelling scope limits (what is not modelled), Reinforcement-learning environment wrapper (stretch goal), SPEC.md (original specification)

### Community 107 - "README Market Maker"
Cohesion: 0.33
Nodes (5): Avellaneda-Stoikov model, AvellanedaStoikovMM, BBO feed message, Engine events (Ack, Reject, Trade, Cancelled, Modified), Market-maker stress test

### Community 108 - "README Run and Replay"
Cohesion: 0.31
Nodes (8): configs/default.json, lobsim_replay, lobsim_run, nlohmann/json v3.11.3, orders.log request log, Sample run (default config, seed 42), docs/sample_run plots, summary.json

### Community 109 - "Simulator Agent Slots"
Cohesion: 0.25
Nodes (7): Simulator::Slot, agent, ctx, decision_rng, entry, feed, response

### Community 110 - "Modify Request Semantics"
Cohesion: 0.25
Nodes (6): priority_kept, Modify, new_price, new_qty, order_id, owner

### Community 111 - "Cancelled Event"
Cohesion: 0.25
Nodes (6): Cancelled, order_id, owner, qty_cancelled, reason, aggressor_side

### Community 112 - "Ack Event"
Cohesion: 0.25
Nodes (6): Ack, client_order_id, order_id, owner, AvellanedaStoikovMM::on_ack(), PrintTo()

### Community 113 - "Exchange Output"
Cohesion: 0.25
Nodes (6): ExchangeOutput, events, public_data, top, top_changed, count_bbo()

### Community 114 - "Private Order Observer"
Cohesion: 0.25
Nodes (4): PrivateOrderObserver, delivered, emitted, trade_delivery_order

### Community 116 - "Modified Event"
Cohesion: 0.33
Nodes (5): Modified, new_price, new_qty, order_id, owner

### Community 117 - "Fundamental Value Process"
Cohesion: 0.33
Nodes (3): Fundamental, rng_, step_sd_

### Community 120 - "Noise Trader Spec"
Cohesion: 0.48
Nodes (5): Daniels et al., A Quantitative Model of Trading and Price Formation, Gode & Sunder (1993), Allocative Efficiency of Markets with Zero-Intelligence Traders, NoiseTrader (zero-intelligence Poisson trader), Smith, Farmer, Gillemot, Krishnamurthy: Statistical Theory of the Continuous Double Auction, Zero-Intelligence (ZI) Order Flow

### Community 121 - "Latency Percentiles"
Cohesion: 0.33
Nodes (6): Percentiles, max, p50, p90, p99, p999

### Community 122 - "Order Id Index"
Cohesion: 0.40
Nodes (3): UnknownOrder, indexed_, OrderBook::release()

### Community 123 - "Mid vs Fundamental Chart"
Cohesion: 0.53
Nodes (5): Mid vs fundamental chart (seed 42), Exchange mid series, Fundamental value series, fundamental, mid

### Community 124 - "Simulation Event Kinds"
Cohesion: 0.33
Nodes (6): EventKind, FundamentalStep, MarketDataDelivery, PrivateDelivery, RequestArrival, Wakeup

### Community 125 - "Pool Test Items"
Cohesion: 0.33
Nodes (4): Item, a, b, link

### Community 126 - "Allocation Audit Rows"
Cohesion: 0.40
Nodes (5): AuditRow, allocs, frees, operation, ops

### Community 127 - "Bench Due Orders"
Cohesion: 0.40
Nodes (4): Due, at, id, owner

### Community 129 - "Book Config"
Cohesion: 0.40
Nodes (4): BookConfig, max_price, OrderBook::OrderBook(), ReferenceBook::ReferenceBook()

### Community 130 - "Trade Prints"
Cohesion: 0.40
Nodes (4): TradePrint, aggressor_side, price, qty

### Community 131 - "MM Quote Orders"
Cohesion: 0.40
Nodes (5): QuoteOrder, id, price, remaining, side

### Community 133 - "Cancel Request"
Cohesion: 0.50
Nodes (3): Cancel, order_id, owner

### Community 134 - "Fuzz Regimes"
Cohesion: 0.50
Nodes (4): Regime, Churn, Drain, Grow

### Community 135 - "MM Test New Info"
Cohesion: 0.50
Nodes (4): NewInfo, price, qty, side

## Ambiguous Edges - Review These
- `reservation_price()` → `Accelerating market-maker PnL growth`  [AMBIGUOUS]
  docs/sample_run/mm_pnl.png · relation: conceptually_related_to
- `Negative Lag-1 Return Autocorrelation (about -0.35)` → `Volatility Clustering`  [AMBIGUOUS]
  docs/sample_run/return_acf.png · relation: conceptually_related_to

## Knowledge Gaps
- **612 isolated node(s):** `size`, `count`, `min_pair_ns`, `median_pair_ns`, `mean_read_ns` (+607 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 1122 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **15 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Price` connect `Seeded Differential Fuzz Generator` to `Core Types and Harnesses`, `Book Config`, `ReferenceBook Implementation`, `Trade Prints`, `OrderBook Public API`, `Metrics Observer Core`, `MM Quote Orders`, `Order Log and Replay`, `Metrics Scripted Tests`, `Book Harness and Conservation`, `Order Request Value Types`, `Market Maker Stress Probe`, `MM Test New Info`, `Run Counts and Fills`, `Agent Context and Traders`, `OrderBook Matching Internals`, `Noise Trader`, `Simulation Config Structures`, `Microbenchmark Book Models`, `Noise Trader Tests`, `Order Enums and Takers`, `Market Maker Test Harness`, `Cancel and Modify Tests`, `OrderBook Levels and Links`, `Exchange Wrapper`, `A-S Quote Formulas`, `Book Views and Snapshots`, `Noise Reference Price`, `MM Pending Requests`, `Bench Message Streams`, `BBO Market Data`, `Microbenchmarks`, `Trade Records`, `Aggressor Test Agent`, `Reference Resting Orders`, `MM Working Orders`, `Noise Trader Params`, `Request Handling`, `Modify Request Semantics`, `Modified Event`?**
  _High betweenness centrality (0.130) - this node is a cross-community bridge._
- **Are the 6 inferred relationships involving `OrderBook` (e.g. with `allocation_audit()` and `D-010 Both books share one non-virtual member API`) actually correct?**
  _`OrderBook` has 6 INFERRED edges - model-reasoned connections that need verification._
- **What connects `size`, `count`, `min_pair_ns` to the rest of the system?**
  _612 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Core Types and Harnesses` be split into smaller, more focused modules?**
  _Cohesion score 0.05940204563335956 - nodes in this community are weakly interconnected._
- **Why does `MetricsObserver` connect `Metrics Observer Core` to `Core Types and Harnesses`, `MM Quote Orders`, `Seeded Differential Fuzz Generator`, `Order Request Value Types`, `CMake Build Targets`, `Summary Statistics Output`, `Run Counts and Fills`, `Simulator Components Spec`, `Markout Decomposition`, `Simulator Run Helpers`, `Observer and Event Timing`, `Spread Statistics`, `Metrics Options and Roles`, `Spread Histogram Plot`, `BBO Market Data`, `Trade Records`, `Run Command Line`, `BBO and Spread Capture Rows`, `MM Window Stats`, `MM PnL Chart`?**
  _High betweenness centrality (0.105) - this node is a cross-community bridge._

### Low-confidence Hints
_AMBIGUOUS edges — the extractor was unsure. Verify before acting on these._

- **What is the exact relationship between `reservation_price()` and `Accelerating market-maker PnL growth`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **What is the exact relationship between `Negative Lag-1 Return Autocorrelation (about -0.35)` and `Volatility Clustering`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._