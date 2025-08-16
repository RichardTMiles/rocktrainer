# rocktrainer

## Development

Enable the git hooks to make sure the build passes before pushing:

```
git config core.hooksPath .githooks
```

The pre-push hook runs the same build as the CI workflow. If [`act`](https://github.com/nektos/act) is installed it will execute the GitHub Actions workflow, otherwise it performs a local CMake build.
