# Installing and Using cordmux

A test guide (good example).

- [Before You Start](#before-you-start)
- [Troubleshooting](#troubleshooting)

## Before You Start

### Requirements

- OBS Studio 33 or later
  - The x64 version

Other people in the call don't need to install or set up anything.

## Installation

### 1. Install Vesktop

1. Install Vesktop from the [official website](https://vesktop.dev/).

### 2. Move the folder

1. Move the "cordmux" folder into `C:\ProgramData\obs-studio\plugins`. If a confirmation asks for administrator permission, select [Continue].

### 3. Add cordmux to Vesktop

1. Select [Add to Vesktop].
   - With another Vencord, select [Switch].
2. Select [Create cordmux Scenes].

A line starting with "Connected to Vesktop" or "In a call" appears in the dock. If it doesn't, see "[Not connected to Vesktop](#not-connected-to-vesktop-x)" in "Troubleshooting".

### 4. Set up an alt account

1. Enter it in the [Main Account User ID] box on the [General] page, then select [OK].

## Each Time You Stream

1. Start OBS and Vesktop. The dock shows something like "In a call (4 people)".
2. Select the [Show Name] check box.

## Using cordmux

### Resuming finalizing

- If OBS closed unexpectedly, select [Resume Finalizing] the next time you start OBS.
- In dock notices, the cordmux added to Vesktop is called the "cordmux in Vesktop".

## Troubleshooting

### Not connected to Vesktop

Check the OBS version with [About] on the [Help] menu.

### Call voices are heard twice

Check the "Audio Mixer" in OBS.

### Uninstalling stopped partway

Don't delete the cordmux files until Vesktop's settings are restored. If you delete them, Vesktop puts the official Vencord where they were.

### Nothing in the OBS preview

Check whether the screen shares appear in Vesktop.

### Windows blocked the files

(to be written after testing)

## Updating

1. Close OBS.
2. Start OBS. If "Restart Vesktop" appears, restart Vesktop.

## Uninstalling

Don't delete the cordmux files until uninstalling is finished. If you delete them first, Vesktop puts the official Vencord where they were.

1. On the [About] page, select [Uninstall cordmux]. "Uninstall cordmux?" appears. Select [Uninstall].
2. "Close OBS, then delete the cordmux folder" appears.

## Compatibility

A placeholder line.

## Source Code and Licenses

A placeholder line. This is outside the steps, so the following words are fine here.

Command prompt, terminal, run as administrator, Win+R, build, compile, PowerShell, cmd, npm, pnpm, node, git, click.

$ a dollar sign at the start of a line

The license text is in "[en-US.ini](../good/en-US.ini)".

<!-- zip:source_url -->
