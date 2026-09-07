#include "net/imgfetch.h"

#include "net/net_http.h"

namespace hominka {

ImageFetch::~ImageFetch() { stop(); }

void ImageFetch::start(int threads) {
    if (!threads_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mx_);
        stopping_ = false;
    }
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { worker(); });
}

void ImageFetch::stop() {
    {
        std::lock_guard<std::mutex> lock(mx_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
}

void ImageFetch::want(const std::string& url) {
    // «data:» несе картинку в собі — качати нема чого.
    if (url.empty() || url.compare(0, 5, "data:") == 0) return;
    {
        std::lock_guard<std::mutex> lock(mx_);
        if (!asked_.insert(url).second) return;
        queue_.push_back(url);
    }
    cv_.notify_one();
}

bool ImageFetch::take(std::string* url, std::vector<uint8_t>* data) {
    std::lock_guard<std::mutex> lock(mx_);
    if (done_.empty()) return false;
    *url = std::move(done_.front().first);
    *data = std::move(done_.front().second);
    done_.pop_front();
    return true;
}

size_t ImageFetch::pending() const {
    std::lock_guard<std::mutex> lock(mx_);
    return queue_.size();
}

void ImageFetch::worker() {
    for (;;) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(mx_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            url = std::move(queue_.front());
            queue_.pop_front();
        }

        const HttpResult r = http_get(url, 20);
        // Помилку не перепитуємо: емоут, якого немає, не з'явиться від другого
        // запиту, а рядок і без картинки читається — там лишається його код.
        // Адреса при цьому лишається в asked_, тож черга не крутитиметься.
        if (!r.ok() || r.body.empty()) continue;

        std::vector<uint8_t> bytes(r.body.begin(), r.body.end());
        {
            std::lock_guard<std::mutex> lock(mx_);
            done_.emplace_back(url, std::move(bytes));
        }
    }
}

}  // namespace hominka
