include(FetchContent)

FetchContent_Declare(
    nui
    GIT_REPOSITORY https://github.com/NuiCpp/Nui.git
    GIT_TAG        v3.5.0
)

FetchContent_MakeAvailable(nui)
