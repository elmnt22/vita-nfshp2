#include "controller_art.hpp"
#include "platform.hpp"

#include <png.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

namespace hp2::nx {
namespace {
constexpr char atlasPath[]=HP2_NX_INSTALL_DIRECTORY "/Glyphs/buttons.png";
constexpr char commonDiagram[]=HP2_NX_INSTALL_DIRECTORY "/Glyphs/controller.png";
constexpr std::array<const char*,4> diagrams{{
    HP2_NX_INSTALL_DIRECTORY "/Glyphs/controller30.png",
    HP2_NX_INSTALL_DIRECTORY "/Glyphs/controller31.png",
    HP2_NX_INSTALL_DIRECTORY "/Glyphs/controller32.png",
    HP2_NX_INSTALL_DIRECTORY "/Glyphs/controller33.png",
}};

constexpr std::uint8_t embeddedAtlas[]{
#include "buttons_png.inc"
};

std::vector<std::uint8_t> LoadPng(const char* path,std::uint32_t width,std::uint32_t height)
{
    const std::unique_ptr<std::FILE,decltype(&std::fclose)> file(path ? std::fopen(path,"rb") : nullptr,&std::fclose);
    struct Png {
        png_image image{};
        Png() {image.version=PNG_IMAGE_VERSION;}
        ~Png() {png_image_free(&image);}
    } png;
    const bool opened=path ?
        (file && png_image_begin_read_from_stdio(&png.image,file.get())) :
        png_image_begin_read_from_memory(&png.image,embeddedAtlas,sizeof(embeddedAtlas));
    if(!opened ||
       png.image.width!=width || png.image.height!=height)return {};
    png.image.format=PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(png.image));
    if(!png_image_finish_read(&png.image,nullptr,pixels.data(),0,nullptr))return {};
    // The renderer's texture combiner reads alpha in the game's 0..128 units.
    for(std::size_t i=3;i<pixels.size();i+=4)
        pixels[i]=static_cast<std::uint8_t>((pixels[i]*128u+127u)/255u);
    return pixels;
}
}

void OverrideControllerTexture(std::uint32_t key,std::uint32_t width,std::uint32_t height,
    std::vector<std::uint8_t>& rgba)
{
    constexpr std::uint32_t buttonsOut=0xb85de386;
    constexpr std::uint32_t controller30=0x411921c6;
    if(key==buttonsOut && width==128 && height==256) {
        auto pixels=LoadPng(atlasPath,width,height);
        if(pixels.empty())pixels=LoadPng(nullptr,width,height);
        if(!pixels.empty())rgba.swap(pixels);
    } else if(key>=controller30 && key-controller30<diagrams.size() &&
              width==256 && height==256) {
        constexpr std::uint32_t diagramHeight=152;
        auto pixels=LoadPng(diagrams[key-controller30],width,diagramHeight);
        if(pixels.empty())pixels=LoadPng(commonDiagram,width,diagramHeight);
        if(!pixels.empty()) {
            std::fill(rgba.begin(),rgba.end(),0);
            std::copy(pixels.begin(),pixels.end(),rgba.begin());
        }
    }
}
}
