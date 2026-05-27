#include <cmath>

#include "check.hpp"
#include "rudp/congestion.hpp"
#include "rudp/rtt.hpp"

int main() {
    using rudp::CongestionController;
    using rudp::RttEstimator;
    using Ms = RttEstimator::Millis;

    {
        RttEstimator rtt(Ms(300), Ms(40), Ms(4000));
        CHECK(!rtt.has_sample());
        CHECK_EQ(rtt.rto().count(), 300.0);

        rtt.sample(Ms(100));  // first sample: SRTT = R, RTTVAR = R/2, RTO = R + 4*R/2
        CHECK_EQ(rtt.srtt().count(), 100.0);
        CHECK_EQ(rtt.rttvar().count(), 50.0);
        CHECK_EQ(rtt.rto().count(), 300.0);

        rtt.sample(Ms(100));  // RTTVAR = 0.75*50, SRTT unchanged
        CHECK(std::abs(rtt.rttvar().count() - 37.5) < 1e-9);
        CHECK(std::abs(rtt.srtt().count() - 100.0) < 1e-9);

        for (int i = 0; i < 200; ++i) rtt.sample(Ms(100));
        CHECK(rtt.rto().count() < 105.0);  // variance decays toward zero
        CHECK(rtt.rto().count() >= 100.0);
    }
    {
        RttEstimator rtt(Ms(300), Ms(40), Ms(1000));
        rtt.backoff();
        CHECK_EQ(rtt.rto().count(), 600.0);
        rtt.backoff();
        CHECK_EQ(rtt.rto().count(), 1000.0);  // capped
        rtt.sample(Ms(1));                    // clamped to the floor, backoff cleared
        CHECK_EQ(rtt.rto().count(), 40.0);
    }
    {
        CongestionController cc(64.0, 2.0);
        CHECK_EQ(cc.window(), std::size_t{2});
        CHECK(cc.in_slow_start());
        cc.on_ack(2);  // slow start: +1 per ACKed packet
        CHECK_EQ(cc.cwnd(), 4.0);

        cc.on_fast_loss();  // multiplicative decrease
        CHECK_EQ(cc.cwnd(), 2.0);
        CHECK_EQ(cc.ssthresh(), 2.0);
        CHECK(!cc.in_slow_start());

        const double before = cc.cwnd();  // additive increase: about +1 per window
        cc.on_ack(2);
        CHECK(cc.cwnd() > before && cc.cwnd() < before + 1.01);

        cc.on_timeout();
        CHECK_EQ(cc.window(), std::size_t{1});
        CHECK(cc.in_slow_start());

        for (int i = 0; i < 1000; ++i) cc.on_ack(1);
        CHECK(cc.cwnd() <= 64.0);
    }
    return check::finish("protocol");
}
