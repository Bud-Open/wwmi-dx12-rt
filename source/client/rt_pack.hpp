#pragma once
#include "hsin_asset_layout.hpp"
#include "native_profile_registry.hpp"
#include "rt_mod_source.h"
#include <filesystem>
#include <span>
namespace hsin_rt {
std::vector<std::shared_ptr<const NativeProfile>> read_native_profiles(const std::filesystem::path &);
struct PackStatus {std::uint64_t key=0;std::wstring name;std::string reason;};
struct PackCatalog {
    std::uint64_t load_generation=0,revision=0,native_revision=0;
    std::vector<NativeMeshIntent> intents;
    std::vector<std::shared_ptr<const ModPack>> packs;
    std::vector<PackStatus> status;
};
class PackSource {
    WWMI_RtModReadCatalogFn_v1 catalog_;
    WWMI_RtModReadResourceFn_v1 resource_;
    std::vector<std::shared_ptr<const NativeProfile>> profiles_;
    std::shared_ptr<NativeProfileRegistry> registry_;
public:
    PackSource(WWMI_RtModReadCatalogFn_v1 c,WWMI_RtModReadResourceFn_v1 r,
        std::vector<std::shared_ptr<const NativeProfile>> profiles):catalog_(c),resource_(r),profiles_(std::move(profiles)),registry_(std::make_shared<NativeProfileRegistry>(profiles_)){}
    PackSource(std::shared_ptr<NativeProfileRegistry> registry,WWMI_RtModReadCatalogFn_v1 c,WWMI_RtModReadResourceFn_v1 r)
        :catalog_(c),resource_(r),profiles_(registry->snapshot()->profiles),registry_(std::move(registry)){}
    // The legacy seed view remains stable. New consumers share registry().
    const auto &profiles() const {return profiles_;}
    const auto &registry() const {return registry_;}
    PackCatalog refresh(const PackCatalog &previous) const;
};
}
