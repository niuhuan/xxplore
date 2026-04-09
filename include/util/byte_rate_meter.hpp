#pragma once

#include <cstdint>
#include <deque>

namespace xxplore::util {

class ByteRateMeter {
public:
    void reset() {
        samples_.clear();
        finished_ = false;
    }

    void update(uint64_t totalBytes, uint64_t timestampSec) {
        finished_ = false;
        if (!samples_.empty() && samples_.back().timestampSec == timestampSec) {
            if (totalBytes > samples_.back().totalBytes)
                samples_.back().totalBytes = totalBytes;
        } else {
            samples_.push_back({timestampSec, totalBytes});
        }
        trim(timestampSec);
    }

    void markFinished(uint64_t timestampSec) {
        trim(timestampSec);
        finished_ = true;
    }

    uint64_t rateBytesPerSec(uint64_t timestampSec) const {
        if (samples_.empty())
            return 0;

        std::size_t first = 0;
        while (first + 1 < samples_.size() &&
               timestampSec > samples_[first].timestampSec &&
               timestampSec - samples_[first].timestampSec > 5) {
            ++first;
        }

        const Sample& oldest = samples_[first];
        const Sample& newest = samples_.back();
        if (newest.timestampSec <= oldest.timestampSec)
            return 0;
        if (newest.totalBytes < oldest.totalBytes)
            return 0;

        uint64_t seconds = newest.timestampSec - oldest.timestampSec;
        if (seconds == 0)
            return 0;
        return (newest.totalBytes - oldest.totalBytes) / seconds;
    }

    bool hasRate(uint64_t timestampSec) const {
        if (samples_.size() < 2)
            return false;

        std::size_t first = 0;
        while (first + 1 < samples_.size() &&
               timestampSec > samples_[first].timestampSec &&
               timestampSec - samples_[first].timestampSec > 5) {
            ++first;
        }

        const Sample& oldest = samples_[first];
        const Sample& newest = samples_.back();
        return newest.timestampSec > oldest.timestampSec &&
               newest.totalBytes >= oldest.totalBytes;
    }

    uint64_t latestTotalBytes() const {
        if (samples_.empty())
            return 0;
        return samples_.back().totalBytes;
    }

    bool finished() const { return finished_; }

private:
    struct Sample {
        uint64_t timestampSec = 0;
        uint64_t totalBytes = 0;
    };

    void trim(uint64_t timestampSec) {
        while (samples_.size() > 1 &&
               timestampSec > samples_.front().timestampSec &&
               timestampSec - samples_.front().timestampSec > 5) {
            samples_.pop_front();
        }
    }

    std::deque<Sample> samples_;
    bool finished_ = false;
};

} // namespace xxplore::util
