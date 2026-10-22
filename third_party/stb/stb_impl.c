/* stb implementation unit for bb_core (PNG/JPEG read, PNG write for mods, docs/MODDING.md). Vendored stb v2.30 / v1.16, LICENSE. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG  /* Blender may write glTF textures as JPEG */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
