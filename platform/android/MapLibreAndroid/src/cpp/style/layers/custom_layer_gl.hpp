#pragma once

#include <GLES3/gl3.h>
#include <jni.h>

#include <mln/util/logging.hpp>

#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>

namespace cumbly_gl {

constexpr double kTileSize = 512.0;

inline GLuint compile(const char* label, GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        std::array<char, 1024> log{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        mln::Log::Error(mln::Event::Render, std::string(label) + " shader: " + log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

inline GLuint link(const char* label,
                   const char* vertexSource,
                   const char* fragmentSource,
                   std::initializer_list<const char*> attributes = {}) {
    GLuint vertexShader = compile(label, GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = compile(label, GL_FRAGMENT_SHADER, fragmentSource);
    if (vertexShader == 0 || fragmentShader == 0) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    GLuint index = 0;
    for (const char* attribute : attributes) {
        glBindAttribLocation(program, index++, attribute);
    }
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

inline double mercatorX(double longitude) {
    return (longitude + 180.0) / 360.0;
}

inline double mercatorY(double latitude) {
    const double radians = latitude * M_PI / 180.0;
    return (1.0 - std::log(std::tan(radians) + 1.0 / std::cos(radians)) / M_PI) / 2.0;
}

template <typename State>
std::shared_ptr<State>& handle(jlong state) {
    return *reinterpret_cast<std::shared_ptr<State>*>(state);
}

} // namespace cumbly_gl
