// SPDX-License-Identifier: GPL-3.0-or-later
#include "NullStore.h"

namespace fl::persist {
namespace {

class NullStore final : public IPersistence, private IBlobRepository {
  public:
    IBlobRepository& blobs() override {
        return *this;
    }
    Result flush() override {
        return Result::success();
    }
    void close() override {}
    [[nodiscard]] StoreHealth health() const override {
        return StoreHealth{}; // open = false, every counter zero: nothing was ever promised
    }
    [[nodiscard]] std::string_view backendName() const override {
        return "null";
    }

  private:
    std::optional<std::vector<std::byte>> get(std::string_view) override {
        return std::nullopt;
    }
    bool exists(std::string_view) override {
        return false;
    }
    std::vector<std::string> keys(std::string_view) override {
        return {};
    }
    void put(std::string_view, std::vector<std::byte>) override {}
    void remove(std::string_view) override {}
};

} // namespace

std::unique_ptr<IPersistence> makeNullStore() {
    return std::make_unique<NullStore>();
}

} // namespace fl::persist
