#include "hmp_walt.h"

#include <string.h>

#include <algorithm>
#include <iostream>
#include <numeric>

WaltHmp::Tunables::Tunables() {
    sched_downmigrate         = 85;
    sched_upmigrate           = 95;
    sched_ravg_hist_size      = 5;
    sched_window_stats_policy = WaltHmp::WINDOW_STATS_MAX_RECENT_AVG;
    sched_boost               = 0;
    timer_rate                = 2;
}

WaltHmp::WaltHmp(Cfg cfg)
    : Hmp(cfg), tunables_(cfg.tunables), demand_(0), entry_cnt_(0), max_load_sum_(0), governor_cnt_(0) {
    SetTunables(cfg.tunables);
    memset(sum_history_, 0, sizeof(sum_history_));
    memset(loads_sum_, 0, sizeof(loads_sum_));
}

void WaltHmp::SetTunables(const Tunables &t) {
    tunables_ = t;
    // Hence this threshold is auto-adjusted by a factor
    // equal to max_possible_frequency/current_frequency of a lower capacity CPU
    up_demand_thd_   = little_->model_.max_freq * little_->model_.efficiency * tunables_.sched_upmigrate;
    down_demand_thd_ = little_->model_.max_freq * little_->model_.efficiency * tunables_.sched_downmigrate;
}

// 更新负载滑动窗口，返回预计的负载需求，@in_demand为freq*busy_pct*efficiency
void WaltHmp::update_history(int in_demand) {
    int *         hist    = sum_history_;
    uint64_t      sum     = 0;
    constexpr int samples = 1;
    const int     runtime = in_demand;
    int           max     = 0;
    int           ridx, widx;
    int           avg, demand;

    /* Push new 'runtime' value onto stack */
    widx = tunables_.sched_ravg_hist_size - 1;
    ridx = widx - samples;
    for (; ridx >= 0; --widx, --ridx) {
        hist[widx] = hist[ridx];
        sum += hist[widx];
        if (hist[widx] > max)
            max = hist[widx];
    }

    for (widx = 0; widx < samples && widx < tunables_.sched_ravg_hist_size; widx++) {
        hist[widx] = runtime;
        sum += hist[widx];
        if (hist[widx] > max)
            max = hist[widx];
    }

    if (tunables_.sched_window_stats_policy == WINDOW_STATS_RECENT) {
        demand = runtime;
    } else if (tunables_.sched_window_stats_policy == WINDOW_STATS_MAX) {
        demand = max;
    } else {
        avg = sum / tunables_.sched_ravg_hist_size;
        if (tunables_.sched_window_stats_policy == WINDOW_STATS_AVG)
            demand = avg;
        else
            demand = std::max(avg, runtime);
    }

    demand_ = demand;
    return;
}

// demand : freq * busy_pct * efficiency，walt输出
// load: freq * busy_pct * efficiency
// load 最大值 2500 * 2048 * 100，sum最大值 3000 * 2048 * 400，可能大于UINT32_MAX
int WaltHmp::SchedulerTick(int max_load, const int *loads, int n_load, int now) {
    ++entry_cnt_;
    max_load_sum_ += max_load;
    for (int i = 0; i < n_load; ++i) {
        loads_sum_[i] += loads[i];
    }

    if (entry_cnt_ == tunables_.timer_rate) {
        int max_load_avg = max_load_sum_ / tunables_.timer_rate;

        entry_cnt_    = 0;
        max_load_sum_ = 0;
        memset(loads_sum_, 0, sizeof(loads_sum_));

        update_history(max_load_avg);

        if (demand_ > up_demand_thd_) {
            active_ = big_;
            idle_   = little_;
        } else if (demand_ < down_demand_thd_) {
            active_ = little_;
            idle_   = big_;
        } else {
            ;
        }

        if (tunables_.sched_boost) {
            active_ = big_;
            idle_   = little_;
        }

        // 调频器使用定期负载采样
        idle_->SetBusyPct(0);
        active_->SetBusyPct(LoadToBusyPct(active_, max_load_avg));

        little_->SetCurfreq(governor_little_->InteractiveTimer(little_->GetBusyPct(), governor_cnt_));
        if (cluster_num_ > 1)
            big_->SetCurfreq(governor_big_->InteractiveTimer(big_->GetBusyPct(), governor_cnt_));

        ++governor_cnt_;
    }

    return active_->CalcCapacity();
}
