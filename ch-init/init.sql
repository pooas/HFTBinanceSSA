DROP TABLE IF EXISTS default.hft_market_data;

CREATE TABLE default.hft_market_data (
    timestamp DateTime64(3),
    sequence UInt64,
    price Float64,
    volume Float64,

    -- 🌟 ستون‌های خروجی معماری جدید SSA (برگرفته از SsaFrame)
    ssa_smoothed Float64,         -- سیگنال ترکیب‌شده نهایی
    ssa_slope Float64,            -- مشتق اول (شیب)
    ssa_accel Float64,            -- مشتق دوم (شتاب)
    ssa_macro_trend Float64,      -- روند ماکرو (فیلتر Ehlers با period = dom_cycle × 50)
    ssa_l_fast Int32,             -- طول پنجره سریع
    ssa_l_slow Int32,             -- طول پنجره کند
    ssa_blend_weight Float32,     -- وزن ترکیب دو پایپ‌لاین
    ssa_evr_fast Float32,         -- واریانس توضیح‌داده‌شده سریع
    ssa_evr_slow Float32,         -- واریانس توضیح‌داده‌شده کند
    ssa_eigen_gap Float32,        -- فاصله مقادیر ویژه

    -- ستون‌های قبلی (که در صورت نیاز سیستم‌های دیگر حفظ شده‌اند)
    lambda Float64,
    is_frozen UInt8,
    regime Int8,
    band_upper Float64,
    band_lower Float64,
    pc0 Float64,
    vress Float64,

    -- وضعیت موتور HMM
    hmm_regime Int8,
    hmm_prob_trend Float64,
    hmm_prob_crisis Float64,
    
    value2 Float64,
    dom_cycle Float64,

    -- 🌟 ستون‌های خروجی قانون بیز و استراتژی مقاله
    momentum_signal Float64,      
    regime_weight Float64,        
    gated_momentum Float64,       
    position_size Float64,        
    dynamic_stop_loss Float64,    
    crisis_cap_active UInt8       
) ENGINE = MergeTree()
PARTITION BY toYYYYMMDD(timestamp)
ORDER BY (timestamp, sequence)
SETTINGS index_granularity = 8192;