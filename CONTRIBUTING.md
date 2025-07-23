# Contributing to Claude Code Hooks Dispatcher (cchd)

Thank you for your interest in contributing to cchd! This document provides guidelines and instructions for contributing to the project.

## Code of Conduct

By participating in this project, you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md). Please read it before contributing.

## How to Contribute

### Reporting Issues

Before creating an issue, please:
- Check the [existing issues](https://github.com/sammyjoyce/cchd/issues) to avoid duplicates
- Use the issue templates when available
- Include as much relevant information as possible

When reporting bugs, include:
- Your operating system and version
- Zig version (run `zig version`)
- Steps to reproduce the issue
- Expected vs actual behavior
- Any error messages or logs

### Suggesting Features

Feature requests are welcome! Please:
- Check if the feature has already been requested
- Explain the use case and why it would be valuable
- Consider if it aligns with the project's goals

### Pull Requests

1. **Fork the repository** and create your branch from `main`
2. **Follow the coding style** used throughout the project
3. **Write tests** for new functionality
4. **Update documentation** as needed
5. **Ensure all tests pass** by running `zig build test`
6. **Format your code** with `zig fmt`
7. **Write clear commit messages** following conventional commits

#### Commit Message Format

We follow the [Conventional Commits](https://www.conventionalcommits.org/) specification:

```
<type>(<scope>): <subject>

<body>

<footer>
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `style`: Code style changes (formatting, etc.)
- `refactor`: Code refactoring
- `test`: Test additions or modifications
- `chore`: Maintenance tasks
- `perf`: Performance improvements

Examples:
```
feat(server): add rate limiting support
fix(protocol): handle malformed JSON gracefully
docs(readme): update installation instructions
```

### Development Setup

1. **Install Zig** (0.14.1 or later):
   ```bash
   # macOS
   brew install zig
   
   # Linux
   # See https://ziglang.org/download/
   ```

2. **Clone your fork**:
   ```bash
   git clone https://github.com/yourusername/cchd.git
   cd cchd
   ```

3. **Install dependencies**:
   ```bash
   # macOS
   brew install curl
   
   # Ubuntu/Debian
   sudo apt-get install libcurl4-openssl-dev
   
   # Fedora/RHEL
   sudo dnf install libcurl-devel
   ```

4. **Build the project**:
   ```bash
   zig build
   ```

5. **Run tests**:
   ```bash
   zig build test
   ```

### Testing

- Write tests for new functionality in `test.zig`
- Ensure all existing tests pass
- Test on multiple platforms if possible
- Include integration tests for protocol changes

### Documentation

- Update README.md for user-facing changes
- Update PROTOCOL.md for protocol changes
- Add inline comments for complex logic
- Update example servers if needed

## Project Structure

```
cchd/
├── src/              # Core implementation
│   ├── cchd.c       # Main dispatcher
│   ├── PROTOCOL.md  # Protocol specification
│   └── README.md    # Implementation details
├── examples/         # Example servers
├── build.zig        # Build configuration
├── test.zig         # Test suite
└── install-*.sh     # Installation scripts
```

## Release Process

Releases are automated through GitHub Actions when tags are pushed:

1. Update version in `build.zig.zon`
2. Update CHANGELOG.md (if exists)
3. Create and push a tag:
   ```bash
   git tag -a v0.2.0 -m "Release version 0.2.0"
   git push origin v0.2.0
   ```

## Getting Help

- Check the [documentation](README.md)
- Look through [existing issues](https://github.com/sammyjoyce/cchd/issues)
- Ask questions in issues with the "question" label

## License

By contributing to cchd, you agree that your contributions will be licensed under the MIT License.