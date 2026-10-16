# wlroots compositors (phoc, sway) render on the KMS device by default. On
# this tablet that is the display controller, so Mesa's kmsro path imports
# every client buffer into it, and those imports are never released: about
# 30 MB of pinned GPU memory per app launch, until the OOM killer runs.
# Render on Panfrost's render node instead.
export WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128

# Use GTK 4's GL renderer. Its default prefers Vulkan, and PanVK is not
# usable on this Mali-G71. Correct derivatives on this GPU need the Mesa
# fork's G71 derivative fix (mesa pkgrel >= 3).
export GSK_RENDERER=gl
