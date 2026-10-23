include(FetchContent)

FetchContent_Declare(
    nui
    GIT_REPOSITORY https://github.com/NuiCpp/Nui.git
    # Immutable commit currently corresponding to reviewed upstream tag v3.5.0.
    GIT_TAG        547f34d694acfae7abf802733273113fcacddc98
    GIT_SHALLOW    FALSE
)

FetchContent_MakeAvailable(nui)
