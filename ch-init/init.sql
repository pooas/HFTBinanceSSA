DROP TABLE IF EXISTS default.hft_market_data;

CREATE TABLE default.hft_market_data (
    timestamp DateTime64(3),
    sequence UInt64,
    price Float64,
    volume Float64,
    ssa_trend Float64,
    lambda Float64,
    is_frozen UInt8,
    regime Int8,
    band_upper Float64,
    band_lower Float64,
    pc0 Float64,
    evr Float64,
    vress Float64,
    eigen_gap Float64,
    hmm_regime Int8,
    hmm_prob_trend Float64,
    hmm_prob_crisis Float64
) ENGINE = MergeTree()
PARTITION BY toYYYYMMDD(timestamp)
ORDER BY (timestamp, sequence)
SETTINGS index_granularity = 8192;