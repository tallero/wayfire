#include <wayfire/util/log.hpp>
#include <map>
#include "opengl-priv.hpp"
#include "wayfire/output.hpp"
#include "core-impl.hpp"
#include "config.h"

extern "C"
{
#define static
#include <wlr/render/egl.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/gles2.h>
#undef static
#include <wlr/types/wlr_output.h>
}

#include <glm/gtc/matrix_transform.hpp>

#include "shaders.tpp"

const char* gl_error_string(const GLenum err) {
    switch (err) {
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";
    }
    return "UNKNOWN GL ERROR";
}

void gl_call(const char *func, uint32_t line, const char *glfunc) {
    GLenum err;
    if ((err = glGetError()) == GL_NO_ERROR)
        return;

    LOGE("gles2: function ", glfunc, " in ", func, " line ", line, ": ",
        gl_error_string(glGetError()));
}

namespace OpenGL
{
    /* Different Context is kept for each output */
    /* Each of the following functions uses the currently bound context */
    program_t program, color_program;
    GLuint compile_shader(std::string source, GLuint type)
    {
        GLuint shader = GL_CALL(glCreateShader(type));

        const char *c_src = source.c_str();
        GL_CALL(glShaderSource(shader, 1, &c_src, NULL));

        int s;
#define LENGTH 1024 * 128
        char b1[LENGTH];
        GL_CALL(glCompileShader(shader));
        GL_CALL(glGetShaderiv(shader, GL_COMPILE_STATUS, &s));
        GL_CALL(glGetShaderInfoLog(shader, LENGTH, NULL, b1));

        if (s == GL_FALSE)
        {
            LOGE("Failed to load shader:\n", source,
                "\nCompiler output:\n", b1);
            return -1;
        }

        return shader;
    }

    /* Create a very simple gl program from the given shader sources */
    GLuint compile_program(std::string vertex_source, std::string frag_source)
    {
        auto vertex_shader = compile_shader(vertex_source, GL_VERTEX_SHADER);
        auto fragment_shader = compile_shader(frag_source, GL_FRAGMENT_SHADER);
        auto result_program = GL_CALL(glCreateProgram());
        GL_CALL(glAttachShader(result_program, vertex_shader));
        GL_CALL(glAttachShader(result_program, fragment_shader));
        GL_CALL(glLinkProgram(result_program));

        /* won't be really deleted until program is deleted as well */
        GL_CALL(glDeleteShader(vertex_shader));
        GL_CALL(glDeleteShader(fragment_shader));

        return result_program;
    }

    void init()
    {
        render_begin();
        // enable_gl_synchronuous_debug()
        program.compile(default_vertex_shader_source,
            default_fragment_shader_source);

        color_program.set_simple(compile_program(default_vertex_shader_source,
                color_rect_fragment_source));

        render_end();
    }

    void fini()
    {
        render_begin();
        program.free_resources();
        color_program.free_resources();
        render_end();
    }

    namespace
    {
        wf::output_t *current_output = NULL;
    }

    void bind_output(wf::output_t *output)
    {
        current_output = output;
    }

    void unbind_output(wf::output_t *output)
    {
        current_output = NULL;
    }

    void render_transformed_texture(wf::texture_t tex,
        const gl_geometry& g, const gl_geometry& texg,
        glm::mat4 model, glm::vec4 color, uint32_t bits)
    {
        program.use(tex.type);

        gl_geometry final_g = g;
        if (bits & TEXTURE_TRANSFORM_INVERT_Y)
            std::swap(final_g.y1, final_g.y2);
        if (bits & TEXTURE_TRANSFORM_INVERT_X)
            std::swap(final_g.x1, final_g.x2);

        GLfloat vertexData[] = {
            final_g.x1, final_g.y2,
            final_g.x2, final_g.y2,
            final_g.x2, final_g.y1,
            final_g.x1, final_g.y1,
        };

        GLfloat coordData[] = {
            0.0f, 0.0f,
            1.0f, 0.0f,
            1.0f, 1.0f,
            0.0f, 1.0f,
        };

        if (bits & TEXTURE_USE_TEX_GEOMETRY) {
            coordData[0] = texg.x1; coordData[1] = texg.y2;
            coordData[2] = texg.x2; coordData[3] = texg.y2;
            coordData[4] = texg.x2; coordData[5] = texg.y1;
            coordData[6] = texg.x1; coordData[7] = texg.y1;
        }

        program.set_active_texture(tex);
        program.attrib_pointer("position", 2, 0, vertexData);
        program.attrib_pointer("uvPosition", 2, 0, coordData);
        program.uniformMatrix4f("MVP", model);
        program.uniform4f("color", color);

        GL_CALL(glEnable(GL_BLEND));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
        GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));

        program.deactivate();
    }

    void render_transformed_texture(wf::texture_t texture,
        const wf::geometry_t& geometry, glm::mat4 transform,
        glm::vec4 color, uint32_t bits)
    {
        bits &= ~TEXTURE_USE_TEX_GEOMETRY;

        gl_geometry gg;
        gg.x1 = geometry.x;
        gg.y1 = geometry.y;
        gg.x2 = gg.x1 + geometry.width;
        gg.y2 = gg.y1 + geometry.height;
        render_transformed_texture(texture, gg, {}, transform, color, bits);
    }

    void render_transformed_texture(wf::texture_t texture,
        const wf::framebuffer_t& framebuffer,
        const wf::geometry_t& geometry, glm::vec4 color, uint32_t bits)
    {
        wf::geometry_t actual_geometry = geometry;
        actual_geometry.x += framebuffer.geometry.x;
        actual_geometry.y += framebuffer.geometry.y;
        render_transformed_texture(texture, geometry,
            framebuffer.get_orthographic_projection(), color, bits);
    }

    void render_rectangle(wf::geometry_t geometry, wf::color_t color,
        glm::mat4 matrix)
    {
        color_program.use(wf::TEXTURE_TYPE_RGBA);
        float x = geometry.x, y = geometry.y,
              w = geometry.width, h = geometry.height;

        GLfloat vertexData[] = {
            x, y + h,
            x + w, y + h,
            x + w, y,
            x, y,
        };

        color_program.attrib_pointer("position", 2, 0, vertexData);
        color_program.uniformMatrix4f("MVP", matrix);
        color_program.uniform4f("color", {color.r, color.g, color.b, color.a});

        GL_CALL(glEnable(GL_BLEND));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
        GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));

        color_program.deactivate();
    }

    void render_begin()
    {
        /* No real reason for 10, 10, 0 but it doesn't matter */
        render_begin(10, 10, 0);
    }

    void render_begin(const wf::framebuffer_base_t& fb)
    {
        render_begin(fb.viewport_width, fb.viewport_height, fb.fb);
    }

    void render_begin(int32_t viewport_width, int32_t viewport_height, uint32_t fb)
    {
        if (!current_output && !wlr_egl_is_current(wf::get_core_impl().egl))
            wlr_egl_make_current(wf::get_core_impl().egl, EGL_NO_SURFACE, NULL);

        wlr_renderer_begin(wf::get_core_impl().renderer,
            viewport_width, viewport_height);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fb));
    }

    void clear(wf::color_t col, uint32_t mask)
    {
        GL_CALL(glClearColor(col.r, col.g, col.b, col.a));
        GL_CALL(glClear(mask));
    }

    void render_end()
    {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        wlr_renderer_scissor(wf::get_core().renderer, NULL);
        wlr_renderer_end(wf::get_core().renderer);
    }
}

bool wf::framebuffer_base_t::allocate(int width, int height)
{
    bool first_allocate = false;
    if (fb == (uint32_t)-1)
    {
        first_allocate = true;
        GL_CALL(glGenFramebuffers(1, &fb));
    }

    if (tex == (uint32_t)-1)
    {

        first_allocate = true;
        GL_CALL(glGenTextures(1, &tex));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    }

    bool is_resize = false;
    /* Special case: fb = 0. This occurs in the default workspace streams, we don't resize anything */
    if (fb != 0)
    {
        if (first_allocate || width != viewport_width || height != viewport_height)
        {
            is_resize = true;
            GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));
            GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                    0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
        }
    }

    if (first_allocate)
    {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fb));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D, tex, 0));
    }

    if (is_resize || first_allocate)
    {
        auto status = GL_CALL(glCheckFramebufferStatus(GL_FRAMEBUFFER));
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            LOGE("Failed to initialize framebuffer");
            return false;
        }
    }

    viewport_width = width;
    viewport_height = height;

    GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

    return is_resize || first_allocate;
}

void wf::framebuffer_base_t::copy_state(wf::framebuffer_base_t&& other)
{
    this->viewport_width = other.viewport_width;
    this->viewport_height = other.viewport_height;

    this->fb = other.fb;
    this->tex = other.tex;

    other.reset();
}

wf::framebuffer_base_t::framebuffer_base_t(wf::framebuffer_base_t&& other)
{
    copy_state(std::move(other));
}

wf::framebuffer_base_t& wf::framebuffer_base_t::operator = (wf::framebuffer_base_t&& other)
{
    if (this == &other)
        return *this;

    release();
    copy_state(std::move(other));

    return *this;
}

void wf::framebuffer_base_t::bind() const
{
    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb));
    GL_CALL(glViewport(0, 0, viewport_width, viewport_height));
}

void wf::framebuffer_base_t::scissor(wlr_box box) const
{
    GL_CALL(glEnable(GL_SCISSOR_TEST));
    GL_CALL(glScissor(box.x, viewport_height - box.y - box.height,
                      box.width, box.height));
}

void wf::framebuffer_base_t::release()
{
    if (fb != uint32_t(-1) && fb != 0)
    {
        GL_CALL(glDeleteFramebuffers(1, &fb));
    }

    if (tex != uint32_t(-1) && (fb != 0 || tex != 0))
    {
        GL_CALL(glDeleteTextures(1, &tex));
    }

    reset();
}

void wf::framebuffer_base_t::reset()
{
    fb = -1;
    tex = -1;
    viewport_width = viewport_height = 0;
}

wlr_box wf::framebuffer_t::framebuffer_box_from_damage_box(wlr_box box) const
{
    if (has_nonstandard_transform)
    {
        // TODO: unimplemented, but also unused for now
        LOGE("unimplemented reached: framebuffer_box_from_geometry_box"
            " with has_nonstandard_transform");
        return {0, 0, 0, 0};
    }

    int width = viewport_width, height = viewport_height;
    if (wl_transform & 1)
        std::swap(width, height);

    wlr_box result = box;
    wl_output_transform transform =
        wlr_output_transform_invert((wl_output_transform)wl_transform);

   // LOGI("got %d,%d %dx%d, %d", box.x, box.y, box.width, box.height, wl_transform);
    wlr_box_transform(&result, &box, transform, width, height);
  //  LOGI("tr %d,%d %dx%d", box.x, box.y, box.width, box.height);
    return result;
}

wlr_box wf::framebuffer_t::damage_box_from_geometry_box(wlr_box box) const
{
    box.x = std::floor(box.x * scale);
    box.y = std::floor(box.y * scale);
    box.width = std::ceil(box.width * scale);
    box.height = std::ceil(box.height * scale);

    return box;
}

wlr_box wf::framebuffer_t::framebuffer_box_from_geometry_box(wlr_box box) const
{
    return framebuffer_box_from_damage_box(damage_box_from_geometry_box(box));
}

wf::region_t wf::framebuffer_t::get_damage_region() const
{
    return damage_box_from_geometry_box({0, 0, geometry.width, geometry.height});
}

glm::mat4 wf::framebuffer_t::get_orthographic_projection() const
{
    auto ortho = glm::ortho(1.0f * geometry.x,
        1.0f * geometry.x + 1.0f * geometry.width,
        1.0f * geometry.y + 1.0f * geometry.height,
        1.0f * geometry.y);

    return this->transform * ortho;
}

#define WF_PI 3.141592f

/* look up the actual values of wl_output_transform enum
 * All _flipped transforms have values (regular_transfrom + 4) */
glm::mat4 get_output_matrix_from_transform(wl_output_transform transform)
{
    glm::mat4 scale = glm::mat4(1.0);

    if (transform >= 4)
        scale = glm::scale(scale, {-1, 1, 0});

    /* remove the third bit if it's set */
    uint32_t rotation = transform & (~4);
    glm::mat4 rotation_matrix(1.0);

    if (rotation == WL_OUTPUT_TRANSFORM_90)
        rotation_matrix = glm::rotate(rotation_matrix, -WF_PI / 2.0f, {0, 0, 1});
    if (rotation == WL_OUTPUT_TRANSFORM_180)
        rotation_matrix = glm::rotate(rotation_matrix,  WF_PI,        {0, 0, 1});
    if (rotation == WL_OUTPUT_TRANSFORM_270)
        rotation_matrix = glm::rotate(rotation_matrix,  WF_PI / 2.0f, {0, 0, 1});

    return rotation_matrix * scale;
}

namespace wf
{
wf::texture_t::texture_t() { }
wf::texture_t::texture_t(GLuint tex)
{ this->tex_id = tex; }

wf::texture_t::texture_t(wlr_texture *texture)
{
    assert(wlr_texture_is_gles2(texture));
    wlr_gles2_texture_attribs attribs;
    wlr_gles2_texture_get_attribs(texture, &attribs);

    /* Wayfire Y-inverts by default */
    this->invert_y = !attribs.inverted_y;
    this->target = attribs.target;
    this->tex_id = attribs.tex;

    if (this->target == GL_TEXTURE_2D) {
        this->type = attribs.has_alpha ?
            wf::TEXTURE_TYPE_RGBA : wf::TEXTURE_TYPE_RGBX;
    } else {
        this->type = wf::TEXTURE_TYPE_EXTERNAL;
    }
}
};

namespace OpenGL
{
class program_t::impl
{
  public:
    std::set<int> active_attrs;
    std::set<int> active_attrs_divisors;

    int active_program_idx = 0;

    int id[wf::TEXTURE_TYPE_ALL];
    std::map<std::string, int> uniforms[wf::TEXTURE_TYPE_ALL];

    /** Find the uniform location for the currently bound program */
    int find_uniform_loc(const std::string& name)
    {
        auto it = uniforms[active_program_idx].find(name);
        if (it != uniforms[active_program_idx].end())
            return it->second;

        uniforms[active_program_idx][name] =
            GL_CALL(glGetUniformLocation(id[active_program_idx], name.c_str()));
        return uniforms[active_program_idx][name];
    }

    std::map<std::string, int> attribs[wf::TEXTURE_TYPE_ALL];
    /** Find the attrib location for the currently bound program */
    int find_attrib_loc(const std::string& name)
    {
        auto it = attribs[active_program_idx].find(name);
        if (it != attribs[active_program_idx].end())
            return it->second;

        attribs[active_program_idx][name] =
            GL_CALL(glGetAttribLocation(id[active_program_idx], name.c_str()));
        return attribs[active_program_idx][name];
    }
};

program_t::program_t()
{
    this->priv = std::make_unique<impl> ();
    for (int i = 0; i < wf::TEXTURE_TYPE_ALL; i++)
        this->priv->id[i] = 0;
}

void program_t::set_simple(GLuint program_id, wf::texture_type_t type)
{
    free_resources();
    assert(type < wf::TEXTURE_TYPE_ALL);
    this->priv->id[type] = program_id;
}

program_t::~program_t() {}

static std::string replace_builtin_with(const std::string& source,
    const std::string& builtin, const std::string& with)
{
    size_t pos = source.find(builtin);
    if (pos == std::string::npos)
        return source;

    return source.substr(0, pos) + with + source.substr(pos + builtin.length());
}

static const std::string builtin = "@builtin@";
static const std::string builtin_ext = "@builtin_ext@";
struct texture_type_builtins
{
    std::string builtin;
    std::string builtin_ext;
};

std::map<wf::texture_type_t, texture_type_builtins> builtins = {
    {wf::TEXTURE_TYPE_RGBA, {builtin_rgba_source, ""}},
    {wf::TEXTURE_TYPE_RGBX, {builtin_rgbx_source, ""}},
    {wf::TEXTURE_TYPE_EXTERNAL, {builtin_external_source,
                                    builtin_ext_external_source}},
};

void program_t::compile(const std::string& vertex_source,
    const std::string& fragment_source)
{
    free_resources();

    for (const auto& program_type : builtins)
    {
        auto fragment = replace_builtin_with(fragment_source,
            builtin, program_type.second.builtin);
        fragment =  replace_builtin_with(fragment,
            builtin_ext, program_type.second.builtin_ext);
        this->priv->id[program_type.first] =
            compile_program(vertex_source, fragment);
    }
}

void program_t::free_resources()
{
    for (int i = 0; i < wf::TEXTURE_TYPE_ALL; i++)
    {
        if (this->priv->id[i])
        {
            GL_CALL(glDeleteProgram(priv->id[i]));
            this->priv->id[i] = 0;
        }
    }
}

void program_t::use(wf::texture_type_t type)
{
    if (priv->id[type] == 0)
    {
        throw std::runtime_error("program_t has no program for type "
            + std::to_string(type));
    }

    GL_CALL(glUseProgram(priv->id[type]));
    priv->active_program_idx = type;
}

int program_t::get_program_id(wf::texture_type_t type)
{
    return priv->id[type];
}

void program_t::uniform1i(const std::string& name, int value)
{
    int loc = priv->find_uniform_loc(name);
    GL_CALL(glUniform1i(loc, value));
}

void program_t::uniform1f(const std::string& name, float value)
{
    int loc = priv->find_uniform_loc(name);
    GL_CALL(glUniform1f(loc, value));

}

void program_t::uniform2f(const std::string& name, float x, float y)
{
    int loc = priv->find_uniform_loc(name);
    GL_CALL(glUniform2f(loc, x, y));
}

void program_t::uniform4f(const std::string& name, const glm::vec4& value)
{
    int loc = priv->find_uniform_loc(name);
    GL_CALL(glUniform4f(loc, value.r, value.g, value.b, value.a));
}

void program_t::uniformMatrix4f(const std::string& name, const glm::mat4& value)
{
    int loc = priv->find_uniform_loc(name);
    GL_CALL(glUniformMatrix4fv(loc, 1, GL_FALSE, &value[0][0]));
}

void program_t::attrib_pointer(const std::string& attrib,
    int size, int stride, const void *ptr, GLenum type)
{
    int loc = priv->find_attrib_loc(attrib);
    priv->active_attrs.insert(loc);

    GL_CALL(glEnableVertexAttribArray(loc));
    GL_CALL(glVertexAttribPointer(loc, size, type, GL_FALSE, stride, ptr));
}

void program_t::attrib_divisor(const std::string& attrib, int divisor)
{
    int loc = priv->find_attrib_loc(attrib);
    priv->active_attrs_divisors.insert(loc);
    GL_CALL(glVertexAttribDivisor(loc, divisor));
}

void program_t::set_active_texture(const wf::texture_t& texture)
{
    GL_CALL(glActiveTexture(GL_TEXTURE0));
    GL_CALL(glBindTexture(texture.target, texture.tex_id));
    GL_CALL(glTexParameteri(texture.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR));

    uniform1f("_wayfire_y_base", texture.invert_y ? 1 : 0);
    uniform1f("_wayfire_y_mult", texture.invert_y ? -1 : 1);
}

void program_t::deactivate()
{
    for (int loc : priv->active_attrs_divisors)
    {
        GL_CALL(glVertexAttribDivisor(loc, 0));
    }

    for (int loc : priv->active_attrs)
    {
        GL_CALL(glDisableVertexAttribArray(loc));
    }

    priv->active_attrs_divisors.clear();
    priv->active_attrs.clear();
    GL_CALL(glUseProgram(0));
}

}
