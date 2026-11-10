# CI workflow, waiting to be installed

`MainDistributionPipeline.yml` builds the extension and runs `test/sql/*.test`
on GitHub Actions. It belongs in `.github/workflows/`, but it was committed
here because the tool that pushed this repository was not allowed to create
workflow files.

To install it, from GitHub's web interface or a local clone, move this file:

    git mv ci/MainDistributionPipeline.yml .github/workflows/MainDistributionPipeline.yml
