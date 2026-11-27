# libSceAgcDriver

## add Opt-in draw paths

- [Pipelines built on the draw despite the async switches](../../../core/libs/prx/libSceAgcDriver/Graphics/src/Draw.cpp) (`APS5_ASYNC_PIPELINE_WORDS`, `APS5_ASYNC_DRAW_COMPILE`) - three routes still build a first-use pipeline on the thread that records the work instead of skipping it while a pool builds it: a draw whose fragment stage replays stencil references builds its first reference's pipeline on the draw committer, every layer window after the first of a layered draw builds its own pipeline there, and a split table call dispatch builds its parts' compute pipelines on the queue worker. Not resolved: how to skip a layered draw or a split dispatch once some of its windows or parts have been recorded.
