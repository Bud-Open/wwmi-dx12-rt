#pragma once

namespace hsin_rt {
std::shared_ptr<GeometryAssets> make_default_author_assets(const GeometryAssets &bootstrap);
void record_author_asset_upload(ID3D12GraphicsCommandList4 *,const GeometryAssets &target,const GeometryAssets &bootstrap);
}
