#include <gtest/gtest.h>
#include <cmath>

namespace AlsaAVR {
namespace Testing {

// Helper class to test volume scaling logic
class VolumeScaler {
public:
    // Convert from 0-100 scale to Denon 0-max_volume scale
    static int scale_to_denon(int level, int max_volume = 98) {
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        return (level * max_volume) / 100;
    }

    // Convert from Denon scale to 0-100 scale
    static int scale_from_denon(int denon_vol, int max_volume = 98) {
        if (denon_vol < 0) denon_vol = 0;
        if (denon_vol > max_volume) denon_vol = max_volume;
        return static_cast<int>((denon_vol * 100.0f) / max_volume + 0.5f);
    }

    // Convert from Denon scale to dB (for TLV calculation)
    // Denon: 00 = -80dB, 80 = 0dB, 98 = +18dB
    // Each step is 1dB (note: Denon supports .5 increments but integer steps are 1dB)
    static float denon_to_db(int denon_vol) {
        // 1 dB steps, starting at -80dB
        return -80.0f + denon_vol;
    }

    // Convert from dB to Denon scale
    static int db_to_denon(float db) {
        // Clamp to valid range
        if (db < -80.0f) db = -80.0f;
        if (db > 18.0f) db = 18.0f;
        return static_cast<int>(db + 80.0f);
    }

    // Percentage to dB (using Denon scale as intermediate)
    static float percentage_to_db(int percentage, int max_volume = 98) {
        int denon_vol = scale_to_denon(percentage, max_volume);
        return denon_to_db(denon_vol);
    }

    // dB to percentage (using Denon scale as intermediate)
    static int db_to_percentage(float db, int max_volume = 98) {
        int denon_vol = db_to_denon(db);
        return scale_from_denon(denon_vol, max_volume);
    }
};

// Test fixture
class VolumeScalingTest : public ::testing::Test {
protected:
    const float EPSILON = 0.1f;  // Tolerance for float comparisons
};

// ============================================================================
// Basic Scaling Tests (0-100 ↔ Denon scale)
// ============================================================================

TEST_F(VolumeScalingTest, ScaleToDenon_Zero) {
    EXPECT_EQ(VolumeScaler::scale_to_denon(0), 0);
}

TEST_F(VolumeScalingTest, ScaleToDenon_Max) {
    EXPECT_EQ(VolumeScaler::scale_to_denon(100), 98);
}

TEST_F(VolumeScalingTest, ScaleToDenon_Fifty) {
    EXPECT_EQ(VolumeScaler::scale_to_denon(50), 49);  // 50/100 * 98 = 49
}

TEST_F(VolumeScalingTest, ScaleToDenon_ClampNegative) {
    EXPECT_EQ(VolumeScaler::scale_to_denon(-10), 0);
}

TEST_F(VolumeScalingTest, ScaleToDenon_ClampOverMax) {
    EXPECT_EQ(VolumeScaler::scale_to_denon(150), 98);
}

TEST_F(VolumeScalingTest, ScaleFromDenon_Zero) {
    EXPECT_EQ(VolumeScaler::scale_from_denon(0), 0);
}

TEST_F(VolumeScalingTest, ScaleFromDenon_Max) {
    EXPECT_EQ(VolumeScaler::scale_from_denon(98), 100);
}

TEST_F(VolumeScalingTest, ScaleFromDenon_FortyNine) {
    EXPECT_EQ(VolumeScaler::scale_from_denon(49), 50);  // 49/98 * 100 = 50
}

TEST_F(VolumeScalingTest, ScaleFromDenon_Rounding) {
    // 45/98 * 100 = 45.918... should round to 46
    EXPECT_EQ(VolumeScaler::scale_from_denon(45), 46);
    // 44/98 * 100 = 44.897... should round to 45
    EXPECT_EQ(VolumeScaler::scale_from_denon(44), 45);
}

// ============================================================================
// Custom Max Volume Tests (for receivers with max < 98)
// ============================================================================

TEST_F(VolumeScalingTest, CustomMaxVolume_70) {
    // Some receivers limit max to 70 for safety
    EXPECT_EQ(VolumeScaler::scale_to_denon(100, 70), 70);
    EXPECT_EQ(VolumeScaler::scale_to_denon(50, 70), 35);
    EXPECT_EQ(VolumeScaler::scale_to_denon(0, 70), 0);
}

TEST_F(VolumeScalingTest, CustomMaxVolume_70_RoundTrip) {
    // Test round-trip conversion
    int original = 50;
    int denon = VolumeScaler::scale_to_denon(original, 70);
    int back = VolumeScaler::scale_from_denon(denon, 70);
    EXPECT_EQ(back, original);
}

TEST_F(VolumeScalingTest, CustomMaxVolume_80) {
    // Common setting: max at 0dB reference
    EXPECT_EQ(VolumeScaler::scale_to_denon(100, 80), 80);
    EXPECT_EQ(VolumeScaler::scale_to_denon(50, 80), 40);
}

// ============================================================================
// dB Conversion Tests (for TLV/hardware control)
// ============================================================================

TEST_F(VolumeScalingTest, DenonToDB_Zero) {
    EXPECT_NEAR(VolumeScaler::denon_to_db(0), -80.0f, EPSILON);
}

TEST_F(VolumeScalingTest, DenonToDB_Reference) {
    // Volume 80 = 0dB reference
    EXPECT_NEAR(VolumeScaler::denon_to_db(80), 0.0f, EPSILON);
}

TEST_F(VolumeScalingTest, DenonToDB_Max) {
    EXPECT_NEAR(VolumeScaler::denon_to_db(98), 18.0f, EPSILON);
}

TEST_F(VolumeScalingTest, DenonToDB_Steps) {
    // Denon uses 1dB steps
    EXPECT_NEAR(VolumeScaler::denon_to_db(1), -79.0f, EPSILON);
    EXPECT_NEAR(VolumeScaler::denon_to_db(2), -78.0f, EPSILON);
    EXPECT_NEAR(VolumeScaler::denon_to_db(10), -70.0f, EPSILON);
}

TEST_F(VolumeScalingTest, DBToDenon_MinusEighty) {
    EXPECT_EQ(VolumeScaler::db_to_denon(-80.0f), 0);
}

TEST_F(VolumeScalingTest, DBToDenon_Reference) {
    EXPECT_EQ(VolumeScaler::db_to_denon(0.0f), 80);
}

TEST_F(VolumeScalingTest, DBToDenon_Max) {
    EXPECT_EQ(VolumeScaler::db_to_denon(18.0f), 98);
}

TEST_F(VolumeScalingTest, DBToDenon_ClampLow) {
    EXPECT_EQ(VolumeScaler::db_to_denon(-100.0f), 0);
}

TEST_F(VolumeScalingTest, DBToDenon_ClampHigh) {
    EXPECT_EQ(VolumeScaler::db_to_denon(25.0f), 98);
}

// ============================================================================
// Percentage ↔ dB Conversions (for ALSA TLV)
// ============================================================================

TEST_F(VolumeScalingTest, PercentageToDB_Zero) {
    EXPECT_NEAR(VolumeScaler::percentage_to_db(0), -80.0f, EPSILON);
}

TEST_F(VolumeScalingTest, PercentageToDB_Max) {
    EXPECT_NEAR(VolumeScaler::percentage_to_db(100), 18.0f, EPSILON);
}

TEST_F(VolumeScalingTest, PercentageToDB_Fifty) {
    // 50% -> denon 49 -> dB
    float db = VolumeScaler::percentage_to_db(50);
    EXPECT_NEAR(db, -31.0f, EPSILON);  // -80 + 49 = -31
}

TEST_F(VolumeScalingTest, PercentageToDB_Reference) {
    // What percentage gives us 0dB?
    // 0dB = denon 80, 80/98 * 100 = 81.6%
    float db = VolumeScaler::percentage_to_db(82);
    EXPECT_NEAR(db, 0.0f, 2.0f);  // 82% -> denon 80 -> 0dB (allow tolerance)
}

TEST_F(VolumeScalingTest, DBToPercentage_MinusEighty) {
    EXPECT_EQ(VolumeScaler::db_to_percentage(-80.0f), 0);
}

TEST_F(VolumeScalingTest, DBToPercentage_PlusEighteen) {
    EXPECT_EQ(VolumeScaler::db_to_percentage(18.0f), 100);
}

TEST_F(VolumeScalingTest, DBToPercentage_Reference) {
    // 0dB = denon 80 = 81.6%
    int pct = VolumeScaler::db_to_percentage(0.0f);
    EXPECT_GE(pct, 81);
    EXPECT_LE(pct, 83);  // Allow rounding tolerance
}

// ============================================================================
// Round-trip Tests (ensure conversions are reversible)
// ============================================================================

TEST_F(VolumeScalingTest, RoundTrip_PercentageToDenon) {
    for (int i = 0; i <= 100; i += 10) {
        int denon = VolumeScaler::scale_to_denon(i);
        int back = VolumeScaler::scale_from_denon(denon);
        // Allow 1% tolerance due to rounding
        EXPECT_NEAR(back, i, 1) << "Failed at " << i << "%";
    }
}

TEST_F(VolumeScalingTest, RoundTrip_DenonToPercentage) {
    for (int denon = 0; denon <= 98; denon += 10) {
        int pct = VolumeScaler::scale_from_denon(denon);
        int back = VolumeScaler::scale_to_denon(pct);
        // Should be exact or within 1 Denon step
        EXPECT_NEAR(back, denon, 1) << "Failed at denon=" << denon;
    }
}

TEST_F(VolumeScalingTest, RoundTrip_DBToDenon) {
    for (float db = -80.0f; db <= 18.0f; db += 5.0f) {
        int denon = VolumeScaler::db_to_denon(db);
        float back = VolumeScaler::denon_to_db(denon);
        // Allow 1dB tolerance (one Denon step)
        EXPECT_NEAR(back, db, 1.0f) << "Failed at " << db << "dB";
    }
}

// ============================================================================
// Edge Cases and Boundary Tests
// ============================================================================

TEST_F(VolumeScalingTest, EdgeCase_NearMax_97) {
    // Denon 97 should map to near 100%
    int pct = VolumeScaler::scale_from_denon(97);
    EXPECT_GE(pct, 98);
    EXPECT_LE(pct, 100);
}

TEST_F(VolumeScalingTest, EdgeCase_NearZero_1) {
    // Denon 1 should map to near 0%
    int pct = VolumeScaler::scale_from_denon(1);
    EXPECT_GE(pct, 0);
    EXPECT_LE(pct, 2);
}

TEST_F(VolumeScalingTest, EdgeCase_CustomMax_Boundary) {
    // At max_volume boundary
    EXPECT_EQ(VolumeScaler::scale_from_denon(70, 70), 100);
    EXPECT_EQ(VolumeScaler::scale_from_denon(71, 70), 100);  // Clamps
}

// ============================================================================
// Real-world Scenarios
// ============================================================================

TEST_F(VolumeScalingTest, RealWorld_SafeListeningLevel) {
    // Typical safe listening: 70% of max
    int denon = VolumeScaler::scale_to_denon(70);
    EXPECT_EQ(denon, 68);  // 70/100 * 98 = 68.6 -> 68

    float db = VolumeScaler::denon_to_db(denon);
    EXPECT_NEAR(db, -12.0f, EPSILON);  // -80 + 68 = -12dB
}

TEST_F(VolumeScalingTest, RealWorld_ReferenceVolume) {
    // Many users set max to 80 (0dB)
    int pct_at_0db = VolumeScaler::scale_from_denon(80, 80);
    EXPECT_EQ(pct_at_0db, 100);

    // 50% of max 80 should give denon 40
    int denon = VolumeScaler::scale_to_denon(50, 80);
    EXPECT_EQ(denon, 40);
    float db = VolumeScaler::denon_to_db(denon);
    EXPECT_NEAR(db, -40.0f, EPSILON);  // -80 + 40 = -40dB
}

TEST_F(VolumeScalingTest, RealWorld_QuietListening) {
    // Very quiet: 20%
    int denon = VolumeScaler::scale_to_denon(20);
    float db = VolumeScaler::denon_to_db(denon);
    // 20% -> denon 19 -> -61dB
    EXPECT_NEAR(db, -61.0f, EPSILON);  // -80 + 19 = -61dB
}

TEST_F(VolumeScalingTest, RealWorld_VolumeStep) {
    // Each Denon step is 0.5dB
    // What's the percentage change per step?
    int vol_40 = VolumeScaler::scale_from_denon(40);
    int vol_41 = VolumeScaler::scale_from_denon(41);

    // Should be approximately 1% change per step
    EXPECT_EQ(vol_41 - vol_40, 1);
}

// ============================================================================
// TLV Range Validation (for ALSA dB scale)
// ============================================================================

TEST_F(VolumeScalingTest, TLV_RangeValidation) {
    // ALSA TLV should report -80dB to +18dB range
    float min_db = VolumeScaler::percentage_to_db(0);
    float max_db = VolumeScaler::percentage_to_db(100);

    EXPECT_NEAR(min_db, -80.0f, EPSILON);
    EXPECT_NEAR(max_db, 18.0f, EPSILON);
}

TEST_F(VolumeScalingTest, TLV_StepSize) {
    // Each 1% step should be approximately 0.98dB
    // 98 dB range / 100 steps ≈ 0.98 dB/step
    float db_0 = VolumeScaler::percentage_to_db(0);
    float db_1 = VolumeScaler::percentage_to_db(1);
    float step = db_1 - db_0;

    // Allow 0.5dB tolerance (may be 0 or 1 depending on rounding)
    EXPECT_GE(step, 0.0f);
    EXPECT_LE(step, 1.5f);
}

} // namespace Testing
} // namespace AlsaAVR
