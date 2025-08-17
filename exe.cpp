#include <iostream>
#include <unordered_map>
#include <string>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <random>
#include <iomanip>
#include <algorithm>
#include <memory>
#include <cassert>
#include <sstream>
#include <fstream>

// Forward declarations
class TokenBucket;
class RateLimiter;
struct Statistics;
struct ClientStatistics;
struct RateLimiterConfig;

// Configuration structure
struct RateLimiterConfig {
    size_t defaultBucketSize = 100;        // Maximum tokens per bucket
    double defaultRefillRate = 10.0;       // Tokens per second
    std::chrono::seconds cleanupInterval{300};  // Client cleanup interval (5 minutes)
    bool enableMetrics = true;              // Enable statistics collection
    bool enableLogging = false;             // Enable detailed logging
    size_t maxClients = 10000;              // Maximum tracked clients
    std::chrono::milliseconds timeoutMs{1}; // Lock timeout
    
    // Per-client custom limits: clientId -> {bucketSize, refillRate}
    std::unordered_map<std::string, std::pair<size_t, double>> clientLimits;
};

// Statistics structures
struct Statistics {
    std::atomic<uint64_t> totalRequests{0};
    std::atomic<uint64_t> acceptedRequests{0};
    std::atomic<uint64_t> rejectedRequests{0};
    std::atomic<double> totalLatency{0.0};
    std::atomic<size_t> activeClients{0};
    
    double getAcceptanceRate() const {
        uint64_t total = totalRequests.load();
        return total > 0 ? (double(acceptedRequests.load()) / total) * 100.0 : 0.0;
    }
    
    double getRejectionRate() const {
        uint64_t total = totalRequests.load();
        return total > 0 ? (double(rejectedRequests.load()) / total) * 100.0 : 0.0;
    }
    
    double getAverageLatency() const {
        uint64_t total = totalRequests.load();
        return total > 0 ? totalLatency.load() / total : 0.0;
    }
};

struct ClientStatistics {
    size_t tokensRemaining;
    size_t bucketSize;
    double refillRate;
    uint64_t totalRequests;
    uint64_t acceptedRequests;
    std::chrono::steady_clock::time_point lastRefill;
    
    double getClientAcceptanceRate() const {
        return totalRequests > 0 ? (double(acceptedRequests) / totalRequests) * 100.0 : 0.0;
    }
};

// Token Bucket implementation
class TokenBucket {
private:
    mutable std::mutex mutex_;
    double tokens_;
    const size_t bucketSize_;
    const double refillRate_; // tokens per second
    std::chrono::steady_clock::time_point lastRefill_;
    
    // Client-specific statistics
    uint64_t totalRequests_;
    uint64_t acceptedRequests_;
    
public:
    TokenBucket(size_t bucketSize, double refillRate) 
        : tokens_(bucketSize), bucketSize_(bucketSize), refillRate_(refillRate),
          lastRefill_(std::chrono::steady_clock::now()),
          totalRequests_(0), acceptedRequests_(0) {}
    
    bool consume(size_t tokensNeeded = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        refillTokens();
        totalRequests_++;
        
        if (tokens_ >= tokensNeeded) {
            tokens_ -= tokensNeeded;
            acceptedRequests_++;
            return true;
        }
        
        return false;
    }
    
    ClientStatistics getStatistics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Update tokens for current statistics
        const_cast<TokenBucket*>(this)->refillTokens();
        
        return {
            static_cast<size_t>(tokens_),
            bucketSize_,
            refillRate_,
            totalRequests_,
            acceptedRequests_,
            lastRefill_
        };
    }
    
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        tokens_ = bucketSize_;
        lastRefill_ = std::chrono::steady_clock::now();
        totalRequests_ = 0;
        acceptedRequests_ = 0;
    }
    
    std::chrono::steady_clock::time_point getLastAccess() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastRefill_;
    }

private:
    void refillTokens() {
        auto now = std::chrono::steady_clock::now();
        auto timePassed = std::chrono::duration<double>(now - lastRefill_).count();
        
        if (timePassed > 0) {
            double tokensToAdd = timePassed * refillRate_;
            tokens_ = std::min(tokens_ + tokensToAdd, static_cast<double>(bucketSize_));
            lastRefill_ = now;
        }
    }
};

// Main Rate Limiter class
class RateLimiter {
private:
    mutable std::mutex clientsMutex_;
    std::unordered_map<std::string, std::unique_ptr<TokenBucket>> clients_;
    RateLimiterConfig config_;
    Statistics stats_;
    
    // Cleanup thread
    std::unique_ptr<std::thread> cleanupThread_;
    std::atomic<bool> shutdownFlag_{false};
    
    // Latency measurement
    std::vector<double> latencyMeasurements_;
    mutable std::mutex latencyMutex_;

public:
    explicit RateLimiter(const RateLimiterConfig& config = RateLimiterConfig()) 
        : config_(config) {
        startCleanupThread();
    }
    
    RateLimiter(size_t bucketSize, double refillRate) {
        config_.defaultBucketSize = bucketSize;
        config_.defaultRefillRate = refillRate;
        startCleanupThread();
    }
    
    ~RateLimiter() {
        shutdown();
    }
    
    bool allowRequest(const std::string& clientId) {
        auto start = std::chrono::high_resolution_clock::now();
        
        bool allowed = allowRequestInternal(clientId);
        
        // Update statistics
        if (config_.enableMetrics) {
            stats_.totalRequests++;
            if (allowed) {
                stats_.acceptedRequests++;
            } else {
                stats_.rejectedRequests++;
            }
            
            // Measure latency
            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();
            
            {
                std::lock_guard<std::mutex> lock(latencyMutex_);
                latencyMeasurements_.push_back(latency);
                // Keep only recent measurements (last 1000)
                if (latencyMeasurements_.size() > 1000) {
                    latencyMeasurements_.erase(latencyMeasurements_.begin());
                }
            }
            
            stats_.totalLatency.fetch_add(latency);
        }
        
        if (config_.enableLogging) {
            std::cout << "[" << getCurrentTimestamp() << "] "
                      << "Client: " << clientId 
                      << ", Request: " << (allowed ? "ALLOWED" : "REJECTED") << std::endl;
        }
        
        return allowed;
    }
    
    bool allowRequests(const std::string& clientId, size_t count) {
        TokenBucket* bucket = getOrCreateBucket(clientId);
        return bucket ? bucket->consume(count) : false;
    }
    
    void updateClientLimit(const std::string& clientId, size_t bucketSize, double refillRate) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        config_.clientLimits[clientId] = {bucketSize, refillRate};
        
        // Remove existing bucket to force recreation with new limits
        clients_.erase(clientId);
    }
    
    void removeClient(const std::string& clientId) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        if (clients_.erase(clientId)) {
            stats_.activeClients--;
        }
    }
    
    Statistics getStatistics() const {
        return stats_;
    }
    
    ClientStatistics getClientStatistics(const std::string& clientId) const {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto it = clients_.find(clientId);
        if (it != clients_.end()) {
            return it->second->getStatistics();
        }
        
        // Return default statistics for non-existent client
        return {0, config_.defaultBucketSize, config_.defaultRefillRate, 0, 0, 
                std::chrono::steady_clock::now()};
    }
    
    std::vector<std::string> getActiveClients() const {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        std::vector<std::string> clients;
        clients.reserve(clients_.size());
        
        for (const auto& pair : clients_) {
            clients.push_back(pair.first);
        }
        
        return clients;
    }
    
    std::vector<double> getLatencyPercentiles() const {
        std::lock_guard<std::mutex> lock(latencyMutex_);
        
        if (latencyMeasurements_.empty()) {
            return {0.0, 0.0, 0.0, 0.0, 0.0}; // P50, P90, P95, P99, P99.9
        }
        
        auto measurements = latencyMeasurements_;
        std::sort(measurements.begin(), measurements.end());
        
        auto getPercentile = [&](double percentile) {
            size_t index = static_cast<size_t>((percentile / 100.0) * (measurements.size() - 1));
            return measurements[index];
        };
        
        return {
            getPercentile(50.0),   // P50
            getPercentile(90.0),   // P90
            getPercentile(95.0),   // P95
            getPercentile(99.0),   // P99
            getPercentile(99.9)    // P99.9
        };
    }
    
    void cleanup() {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        
        auto now = std::chrono::steady_clock::now();
        auto threshold = now - config_.cleanupInterval;
        
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->second->getLastAccess() < threshold) {
                it = clients_.erase(it);
                stats_.activeClients--;
            } else {
                ++it;
            }
        }
    }
    
    void reset() {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        
        for (auto& pair : clients_) {
            pair.second->reset();
        }
        
        // Reset statistics
        stats_.totalRequests = 0;
        stats_.acceptedRequests = 0;
        stats_.rejectedRequests = 0;
        stats_.totalLatency = 0.0;
        
        // Clear latency measurements
        {
            std::lock_guard<std::mutex> latencyLock(latencyMutex_);
            latencyMeasurements_.clear();
        }
    }
    
    void printDetailedStats() const {
        auto stats = getStatistics();
        auto percentiles = getLatencyPercentiles();
        
        std::cout << "\n=== Rate Limiter Statistics ===\n";
        std::cout << "Total Requests:     " << stats.totalRequests << "\n";
        std::cout << "Accepted Requests:  " << stats.acceptedRequests << " (" 
                  << std::fixed << std::setprecision(1) << stats.getAcceptanceRate() << "%)\n";
        std::cout << "Rejected Requests:  " << stats.rejectedRequests << " (" 
                  << std::fixed << std::setprecision(1) << stats.getRejectionRate() << "%)\n";
        std::cout << "Active Clients:     " << stats.activeClients << "\n";
        std::cout << "Average Latency:    " << std::fixed << std::setprecision(3) 
                  << stats.getAverageLatency() << " ms\n";
        
        std::cout << "\nLatency Percentiles:\n";
        std::cout << "  P50:  " << std::fixed << std::setprecision(3) << percentiles[0] << " ms\n";
        std::cout << "  P90:  " << std::fixed << std::setprecision(3) << percentiles[1] << " ms\n";
        std::cout << "  P95:  " << std::fixed << std::setprecision(3) << percentiles[2] << " ms\n";
        std::cout << "  P99:  " << std::fixed << std::setprecision(3) << percentiles[3] << " ms\n";
        std::cout << "  P99.9:" << std::fixed << std::setprecision(3) << percentiles[4] << " ms\n";
        std::cout << "================================\n\n";
    }

private:
    bool allowRequestInternal(const std::string& clientId) {
        TokenBucket* bucket = getOrCreateBucket(clientId);
        return bucket ? bucket->consume() : false;
    }
    
    TokenBucket* getOrCreateBucket(const std::string& clientId) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        
        auto it = clients_.find(clientId);
        if (it != clients_.end()) {
            return it->second.get();
        }
        
        // Check if we've reached the maximum number of clients
        if (clients_.size() >= config_.maxClients) {
            return nullptr;
        }
        
        // Get client-specific limits or use defaults
        size_t bucketSize = config_.defaultBucketSize;
        double refillRate = config_.defaultRefillRate;
        
        auto limitIt = config_.clientLimits.find(clientId);
        if (limitIt != config_.clientLimits.end()) {
            bucketSize = limitIt->second.first;
            refillRate = limitIt->second.second;
        }
        
        auto bucket = std::make_unique<TokenBucket>(bucketSize, refillRate);
        TokenBucket* bucketPtr = bucket.get();
        
        clients_[clientId] = std::move(bucket);
        stats_.activeClients++;
        
        return bucketPtr;
    }
    
    void startCleanupThread() {
        cleanupThread_ = std::make_unique<std::thread>([this]() {
            while (!shutdownFlag_.load()) {
                std::this_thread::sleep_for(config_.cleanupInterval);
                if (!shutdownFlag_.load()) {
                    cleanup();
                }
            }
        });
    }
    
    void shutdown() {
        shutdownFlag_ = true;
        if (cleanupThread_ && cleanupThread_->joinable()) {
            cleanupThread_->join();
        }
    }
    
    std::string getCurrentTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
        ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }
};

// Benchmark and testing utilities
class BenchmarkSuite {
public:
    struct BenchmarkConfig {
        size_t numClients = 100;
        size_t requestsPerClient = 100;
        std::chrono::milliseconds test
