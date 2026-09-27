# Explicit public surface: every foundational folder has a tested consumer.
set(_foundation_headers
    API/Services.h Core/Assert/Contract.h Core/Event/Event.h Core/File/File.h
    Core/IO/BinaryCursor.h Core/Path/PathRoot.h Core/Platform/PlatformProfile.h
    Core/Property/PropertyLens.h Core/Result/Result.h Core/Task/TaskGroup.h
    Core/Time/Deadline.h Core/Version/Version.h Debug/Diagnostics.h
    Graph/DependencyGraph.h Scene/Scene.h Scripting/Bindings.h SDL/HostSession.h
    Vulkan/Timeline.h)
set(_foundation_sources
    src/Graph/DependencyGraph.cpp src/Scene/Scene.cpp src/Scripting/Bindings.cpp
    src/SDL/HostSession.cpp)
