use_relative_paths = True

vars = {
  # Three lines of non-changing comments so that
  # the commit queue can handle CLs rolling different
  # dependencies without interference from each other.
  'sk_tool_revision': 'git_revision:0d4c52fc72a9b8a9daa641bdc6fc8b2ab4fa235e',

  # ninja CIPD package version.
  # https://chrome-infra-packages.appspot.com/p/infra/3pp/tools/ninja
  'ninja_version': 'version:2@1.8.2.chromium.3',

  # googlefonts_testdata CIPD package version
  # https://chrome-infra-packages.appspot.com/p/chromium/third_party/googlefonts_testdata/
  'googlefonts_testdata_version': 'version:20230913',
}

# If you modify this file, you will need to regenerate the Bazel version of this file (bazel/deps.bzl).
# To do so, run:
#     bazelisk run //bazel/deps_parser
#
# To apply the changes for the GN build, you will need to resync the git repositories using:
#     ./tools/git-sync-deps
deps = {
  "buildtools"                                   : "https://chromium.googlesource.com/chromium/src/buildtools.git@b138e6ce86ae843c42a1a08f37903207bebcca75",
  "third_party/externals/angle2"                 : "https://chromium.googlesource.com/angle/angle.git@800ca8d38b69203eaacd0c4553c0b1fe26ddd25e",
  "third_party/externals/brotli"                 : "https://skia.googlesource.com/external/github.com/google/brotli.git@6d03dfbedda1615c4cba1211f8d81735575209c8",
  "third_party/externals/d3d12allocator"         : "https://skia.googlesource.com/external/github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git@169895d529dfce00390a20e69c2f516066fe7a3b",
  # Dawn requires jinja2 and markupsafe for the code generator, tint for SPIRV compilation, and abseil for string formatting.
  # When the Dawn revision is updated these should be updated from the Dawn DEPS as well.
  "third_party/externals/dawn"                   : "https://dawn.googlesource.com/dawn.git@b65ac9ea6f70f7043710b0e0d902e909225bd9aa",
  "third_party/externals/jinja2"                 : "https://chromium.googlesource.com/chromium/src/third_party/jinja2@e2d024354e11cc6b041b0cff032d73f0c7e43a07",
  "third_party/externals/markupsafe"             : "https://chromium.googlesource.com/chromium/src/third_party/markupsafe@0bad08bb207bbfc1d6f3bbc82b9242b0c50e5794",
  "third_party/externals/abseil-cpp"             : "https://skia.googlesource.com/external/github.com/abseil/abseil-cpp.git@65a55c2ba891f6d2492477707f4a2e327a0b40dc",
  "third_party/externals/dng_sdk"                : "https://android.googlesource.com/platform/external/dng_sdk.git@c8d0c9b1d16bfda56f15165d39e0ffa360a11123",
  "third_party/externals/egl-registry"           : "https://skia.googlesource.com/external/github.com/KhronosGroup/EGL-Registry@b055c9b483e70ecd57b3cf7204db21f5a06f9ffe",
  "third_party/externals/emsdk"                  : "https://skia.googlesource.com/external/github.com/emscripten-core/emsdk.git@a896e3d066448b3530dbcaa48869fafefd738f57",
  "third_party/externals/expat"                  : "https://chromium.googlesource.com/external/github.com/libexpat/libexpat.git@441f98d02deafd9b090aea568282b28f66a50e36",
  "third_party/externals/freetype"               : "https://chromium.googlesource.com/chromium/src/third_party/freetype2.git@a46424228f0998a72c715f32e18dca8a7a764c1f",
  "third_party/externals/harfbuzz"               : "https://chromium.googlesource.com/external/github.com/harfbuzz/harfbuzz.git@b74a7ecc93e283d059df51ee4f46961a782bcdb8",
  "third_party/externals/highway"                : "https://chromium.googlesource.com/external/github.com/google/highway.git@424360251cdcfc314cfc528f53c872ecd63af0f0",
  "third_party/externals/icu"                    : "https://chromium.googlesource.com/chromium/deps/icu.git@364118a1d9da24bb5b770ac3d762ac144d6da5a4",
  "third_party/externals/icu4x"                  : "https://chromium.googlesource.com/external/github.com/unicode-org/icu4x.git@bcf4f7198d4dc5f3127e84a6ca657c88e7d07a13",
  "third_party/externals/imgui"                  : "https://skia.googlesource.com/external/github.com/ocornut/imgui.git@55d35d8387c15bf0cfd71861df67af8cfbda7456",
  "third_party/externals/libavif"                : "https://skia.googlesource.com/external/github.com/AOMediaCodec/libavif.git@55aab4ac0607ab651055d354d64c4615cf3d8000",
  "third_party/externals/libgav1"                : "https://chromium.googlesource.com/codecs/libgav1.git@5cf722e659014ebaf2f573a6dd935116d36eadf1",
  "third_party/externals/libgrapheme"            : "https://skia.googlesource.com/external/github.com/FRIGN/libgrapheme/@c0cab63c5300fa12284194fbef57aa2ed62a94c0",
  "third_party/externals/libjpeg-turbo"          : "https://chromium.googlesource.com/chromium/deps/libjpeg_turbo.git@ccfbe1c82a3b6dbe8647ceb36a3f9ee711fba3cf",
  "third_party/externals/libjxl"                 : "https://chromium.googlesource.com/external/gitlab.com/wg1/jpeg-xl.git@a205468bc5d3a353fb15dae2398a101dff52f2d3",
  "third_party/externals/libpng"                 : "https://skia.googlesource.com/third_party/libpng.git@ed217e3e601d8e462f7fd1e04bed43ac42212429",
  "third_party/externals/libwebp"                : "https://chromium.googlesource.com/webm/libwebp.git@845d5476a866141ba35ac133f856fa62f0b7445f",
  "third_party/externals/libyuv"                 : "https://chromium.googlesource.com/libyuv/libyuv.git@d248929c059ff7629a85333699717d7a677d8d96",
  "third_party/externals/microhttpd"             : "https://android.googlesource.com/platform/external/libmicrohttpd@748945ec6f1c67b7efc934ab0808e1d32f2fb98d",
  "third_party/externals/oboe"                   : "https://chromium.googlesource.com/external/github.com/google/oboe.git@b02a12d1dd821118763debec6b83d00a8a0ee419",
  "third_party/externals/opengl-registry"        : "https://skia.googlesource.com/external/github.com/KhronosGroup/OpenGL-Registry@14b80ebeab022b2c78f84a573f01028c96075553",
  "third_party/externals/perfetto"               : "https://android.googlesource.com/platform/external/perfetto@93885509be1c9240bc55fa515ceb34811e54a394",
  "third_party/externals/piex"                   : "https://android.googlesource.com/platform/external/piex.git@bb217acdca1cc0c16b704669dd6f91a1b509c406",
  "third_party/externals/swiftshader"            : "https://swiftshader.googlesource.com/SwiftShader@cea33ab2d5ad50cd7f1881fb9c36ffcaecdd69fc",
  "third_party/externals/vulkanmemoryallocator"  : "https://chromium.googlesource.com/external/github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator@a6bfc237255a6bac1513f7c1ebde6d8aed6b5191",
  # vulkan-deps is a meta-repo containing several interdependent Khronos Vulkan repositories.
  # When the vulkan-deps revision is updated, those repos (spirv-*, vulkan-*) should be updated as well.
  "third_party/externals/vulkan-deps"            : "https://chromium.googlesource.com/vulkan-deps@83e9eca04a1ba6f6e13263bb649d9d52d7e0fc52",
  "third_party/externals/spirv-cross"            : "https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Cross@b8fcf307f1f347089e3c46eb4451d27f32ebc8d3",
  "third_party/externals/spirv-headers"          : "https://skia.googlesource.com/external/github.com/KhronosGroup/SPIRV-Headers.git@2acb319af38d43be3ea76bfabf3998e5281d8d12",
  "third_party/externals/spirv-tools"            : "https://skia.googlesource.com/external/github.com/KhronosGroup/SPIRV-Tools.git@581279dedd59d8353322fc2d61be07ccdcad0f13",
  "third_party/externals/vello"                  : "https://skia.googlesource.com/external/github.com/linebender/vello.git@6938a2893d6a2ba658709d1d04720f6c6033700f",
  "third_party/externals/vulkan-headers"         : "https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Headers@e3c37e6e184a232e10b01dff5a065ce48c047f88",
  "third_party/externals/vulkan-tools"           : "https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Tools@345af476e583366352e014ee8e43fc5ddf421ab9",
  "third_party/externals/vulkan-utility-libraries": "https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Utility-Libraries@1b07de9a3a174b853833f7f87a824f20604266b9",
  "third_party/externals/unicodetools"           : "https://chromium.googlesource.com/external/github.com/unicode-org/unicodetools@66a3fa9dbdca3b67053a483d130564eabc5fe095",
  #"third_party/externals/v8"                     : "https://chromium.googlesource.com/v8/v8.git@5f1ae66d5634e43563b2d25ea652dfb94c31a3b4",
  "third_party/externals/wuffs"                  : "https://skia.googlesource.com/external/github.com/google/wuffs-mirror-release-c.git@e3f919ccfe3ef542cfc983a82146070258fb57f8",
  "third_party/externals/zlib"                   : "https://chromium.googlesource.com/chromium/src/third_party/zlib@646b7f569718921d7d4b5b8e22572ff6c76f2596",

  'bin': {
    'packages': [
      {
        'package': 'skia/tools/sk/${{platform}}',
        'version': Var('sk_tool_revision'),
      },
      {
        'package': 'infra/3pp/tools/ninja/${{platform}}',
        'version': Var('ninja_version'),
      }
    ],
    'dep_type': 'cipd',
  },
}
