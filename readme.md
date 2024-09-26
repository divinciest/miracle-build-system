  Miracle Build System - README

Miracle Build System
====================

Overview
--------

The Miracle Build System is a powerful, flexible, and independent build tool designed to streamline the compilation and linking process for C/C++ projects. Initially developed as part of the Umbrella Game Engine, it has evolved into a standalone system that can be used for any C/C++ software project, especially those with complex dependencies or specialized requirements like game development.

Miracle is designed to be easy to use, with a simple command-line interface and a Lua-based scripting interface that provides powerful features like automatic dependency management, file matching, and customizable build steps. It also includes a comprehensive error reporting system that captures and logs all compilation and linking errors, making it easy to identify and fix issues.


The system has utility features such as searching libraries, allowing full customization of the build process, and support for cross-platform builds, making it a powerful tool for game development and other complex software projects.


Key Features
------------

*   **Lua-based Configuration**: Offers an intuitive and powerful Lua scripting interface for build configuration, making it both easy to use and highly customizable.
*   **Cross-Platform Support**: Compatible with multiple platforms, with specific optimizations for Windows.
*   **Flexible Target Types**: Build various target types, including console applications, static libraries, and shared libraries.
*   **Intelligent Dependency Management**: Automatically tracks dependencies and performs incremental builds for efficient development cycles.
*   **Advanced File Matching**: Includes powerful regex-based file matching to easily include source files in your build.
*   **Customizable Build Process**: Provides fine-grained control over compiler and linker flags.
*   **Comprehensive Error Reporting**: Captures and reports detailed logs of compilation and linking errors.
*   **Extensible Architecture**: Supports custom toolchains and build steps through Lua scripting.

Core Components
---------------

### MiracleExecuter

The central engine that orchestrates the build process, interpreting Lua scripts to manage compilation and linking stages for projects.

### Target

Represents a build target, such as an executable, static library, or shared library. The `Target` class encapsulates all the necessary information to build a specific component, including source files, output files, and dependency libraries.

### ToolChain

Represents the tools used for building, including compilers and linkers. Miracle comes with pre-configured toolchains and allows for easy customization to support different compilers or specialized environments.

### Compiler

Abstracts the functionality of C and C++ compilers, providing seamless integration and switching between different compiler versions or vendors as needed.

### Linker

Manages the linking process, supporting the creation of executables and libraries with fully customizable linking flags.

Getting Started
---------------

1.  Set up the `MIRACLE_HOME` environment variable pointing to the root directory of your Miracle Build System installation.
2.  Create a Lua build script (with a `.mbs` extension) in your project directory. Here’s an example:

    -- Set up the toolchain
    GCC_TOOLCHAIN.CPPCompiler:SetPath(Path.new("g++.exe"))
    GCC_TOOLCHAIN.CCompiler:SetPath(Path.new("gcc.exe"))
    GCC_TOOLCHAIN:SetLinkerPath(Path.new("g++.exe"))
    
    -- Create a new target
    target = Target.new()
    target.BuildToolChain = GCC_TOOLCHAIN
    target.Type = "CONSOLE_APPLICATION"
    target:SetOutputFileName("MyProject.exe")
    
    -- Add source files and include paths
    target:AddFilesMatchsRegex(".*\\.cpp$")
    target:AddIncludePath("include")
    
    -- Build the target
    if (target:Build() ~= 0) then
      print("Build failed. See error log.")
    else
      print("Build succeeded!")
    end
    

3.  Run the Miracle Build System, pointing it to your Lua script:

    MiracleBuild MyProject.mbs

Advanced Features
-----------------

### Custom Build Steps

You can extend the build process by adding custom build steps in your Lua script, enabling actions like asset processing, code generation, or other project-specific tasks.

Extensibility
-------------

The Miracle Build System is highly extensible, enabling you to:

*   Create custom toolchains for specialized environments.
*   Extend the Lua API with your own functions to customize the build process.
*   Integrate other tools or steps into the build pipeline via Lua scripting.

Troubleshooting
---------------

*   Check the console output and logs for detailed information on build failures.
*   Ensure that all required compilers and tools are installed and accessible in your system’s `PATH`.

Example: Static Library Build
-----------------------------

    GCC_TOOLCHAIN.CCompiler:SetPath(Path.new("gcc.exe"))
    GCC_TOOLCHAIN.CPPCompiler:SetPath(Path.new("g++.exe"))
    GCC_TOOLCHAIN.StaticLinker:SetPath(Path.new("ar.exe"))
    GCC_TOOLCHAIN:SetStaticLibraryExtension(".a")
    GCC_TOOLCHAIN:SetObjectFileExtension(".o")
    
    target = Target.new()
    target.BuildToolChain = GCC_TOOLCHAIN
    target.Type = "STATIC_LIBRARY"
    target:SetOutputFileName("libMyLibrary")
    target:SetOutputFolder("lib")
    
    -- Add source files and include paths
    target:AddFilesMatchsRegex(".*\\.cpp$")
    target:AddIncludePath("include")
    
    -- Build the target
    if (target:Build() ~= 0) then
      print("Build failed.")
    else
      print("Static library built successfully!")
    end
    

Contributing
------------

Contributions to the Miracle Build System are welcome! Please see our contributing guidelines for information on how to submit pull requests, report issues, or request features.

License
-------

The Miracle Build System is released under the \[appropriate license\], and we encourage its use and contribution from the community.