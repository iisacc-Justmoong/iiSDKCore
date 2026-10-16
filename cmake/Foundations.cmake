# Explicit public surface: every foundational folder has a tested consumer.
set(_foundation_headers
    Core/Bytes/Bytes.h Core/Text/Utf8.h Core/Text/Text.h Core/Value/Value.h
    API/Services.h Core/Assert/Contract.h Core/Event/Event.h Core/Event/Receiver.h Core/Memory/SharedValue.h Core/Task/Parallel.h Animation/Timeline.h Core/File/File.h
    Core/IO/BinaryCursor.h Core/Serialization/CBOR.h Core/Serialization/Serialization.h Core/Serialization/SharedValue.h Core/Path/PathRoot.h Core/Platform/PlatformProfile.h
    Core/Property/PropertyLens.h Core/Result/Result.h Core/Task/TaskGroup.h
    Core/Time/Deadline.h Core/Time/Time.h Core/Version/Version.h Debug/Diagnostics.h
    Graph/DependencyGraph.h Scene/Scene.h Scripting/Bindings.h SDL/HostSession.h
    Vulkan/Timeline.h)
set(_foundation_sources
    src/Graph/DependencyGraph.cpp src/Scene/Scene.cpp src/Scripting/Bindings.cpp
    src/SDL/HostSession.cpp)
