# libSceAgcDriver

## add Functional

- [Sampled texture release recency](../../../core/libs/prx/libSceAgcDriver/Graphics/src/ShaderResources.cpp) (libSceAgcDriver) - a release for memory pressure (`ReleaseTextureCaches` asked for less than everything) stops its sampled pass at the least recently used entry used in the current or previous present and within `SampledReleaseKeepWindow` (60 s), so the refused allocation goes to host memory. The 60 s is a choice, not measured, made to outlast one title's 15 to 17 s build of a 28,000-texture image table. Before the first present nothing is kept. While presents continue through a long resolve (an animated loading screen), only the last two presents' textures are kept, so the resolve can still evict what it needs next. An allocation with no host memory type to fall back to can now fail where an eviction would have made room. Not measured in game.
