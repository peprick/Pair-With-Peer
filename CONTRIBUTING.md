# Contributing to Pair with Peer

Thanks for considering an improvement. Small, focused pull requests are the easiest to review and merge.

## Local workflow

1. Fork the repository and create a descriptive branch.
2. Build and run the automated checks:

   ```sh
   make
   make test
   make test-e2e
   ```

3. Format changed C++ files with `make format`.
4. Explain the user-visible behavior in the pull request and include a test when practical.

## Code style

- Use C++17 and keep the build warning-free with the flags in the Makefile.
- Put shared protocol and file utilities in the `pwp` namespace.
- Prefer RAII for sockets, files, mutexes, and threads.
- Treat all network input as untrusted: bound sizes, validate values, and handle partial I/O.
- Keep tracker metadata traffic separate from direct peer file transfers.

## Reporting bugs

Include your operating system, compiler version, exact command, relevant log output, and the smallest reproduction you can provide. Please avoid attaching sensitive files or network addresses.
