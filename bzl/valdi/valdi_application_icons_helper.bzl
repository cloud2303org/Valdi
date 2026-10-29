def make_application_icons(platform, src, round_src = None):
    return struct(
        _valdi_application_icons = platform,
        src = src,
        round_src = round_src,
    )

def is_application_icons(value, platform):
    return hasattr(value, "_valdi_application_icons") and value._valdi_application_icons == platform

def convert_icon(ctx, src, output, size, round_icon = False):
    fail("Legacy Valdi application icon conversion was removed with the C++ toolbox; generate icons in the application's Node asset pipeline")

def write_apple_contents_json(ctx, output, images):
    ctx.actions.write(
        output = output,
        content = json.encode_indent({
            "images": images,
            "info": {
                "author": "xcode",
                "version": 1,
            },
        }) + "\n",
    )
