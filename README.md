# Introduction
This repository contains the firmware and related tools for the Purina D1 project.  It's composed of several apps, which have a common board definition and drivers.  Some of the apps are for development and testing purposes, and eventually there will be a final "main" application containing the firmware that runs on the device for the end user.

# Cloning Repo
* make new workspace (ex. my-workspace) folder (this is where a new version of zephyr and ncs will be installed, so use a new folder)
* `west init -m https://github.com/purina-nbm/tracker_fw --mr develop my-workspace` OR
`west init -m git@github.com:purina-nbm/tracker_fw.git --mr develop my-workspace`
* `cd my-workspace`
* `python3 -mvenv .venv`
* `. ./.venv/bin/activate`
* `west update`
* `pip3 install -r zephyr/scripts/requirements.txt`
* `cd purina-d1`

# Building
* `west build -b $BOARD app`, or `cd <app_name> && make` or `cd <app_name> && make flash`

# Release Process
We have several applications in this repository that need to be maintained, and are all on a different release schedule.
Therefore, release versions are based around the the particular application name and version being released, rather
than an incremental release tag for everything.
To create a new release:
 * click on [Releases](https://github.com/CuvertEngineering/purina-d1/releases)
 * Draft a new release
 * Name the release tag <app_name>-x.y.z, where `app_name` is the application you want to create a release for, and `x.y.z` matches the version in CMakeLists.txt.  NOTE: CI will fail if there's a tag mismatch or `app_name` doesn't exist.
 * Name the Release title the same as the new tag name
 * Add release notes, ideally including a description and links to the PR's included in this release relevant to the app since the last release.  NOTE: auto-generating release notes doesn't work in this release process, because it's looking at all new PR's since the last release which may not be relevant depending on the app being released.  We could potentially come up with a PR naming convention and automate this if it becomes burdensome.
 * Publish release.  This should build <app_name> and attach it to the release.

### MHR oct 5 2023 notes
* Had to install 15.2 of the zephyr sdk. ( 16.xx didnt work)
* Had to brew install ccache.
* Remember to install the python requirements from zephyr/scripts

### NMS Nov 4 2024 notes
* Main branch is `develop`.
* The `main` branch is not maintained!
* Release branches are named `release/1.xx`.
