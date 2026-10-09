#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct DiagsessionResource {
    std::string id;
    std::string type;
    std::string name;
    std::string stored_path;
    bool directory = false;
};

class DiagsessionContainer {
public:
    DiagsessionContainer();
    ~DiagsessionContainer();
    // Borrows bytes until this container is destroyed or opened again.
    bool open(std::string_view bytes, std::string& error);
    const std::vector<DiagsessionResource>& resources() const;
    const std::string& format() const;
    const std::string& producer_version() const;
    bool read_resource(size_t index, std::vector<uint8_t>& output, std::string& error,
                       std::function<bool()> cancelled = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
