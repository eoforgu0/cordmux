# Installing and Using cordmux

cordmux is an OBS plugin that brings the screen shares and voices of a Discord call into OBS. It shows the call participants' screen shares in the four slots the OBS canvas is split into, and can also send their voices and screen share audio to your stream.

This guide covers the steps from installation to using cordmux on stream, and what to do when something goes wrong.

- [Before You Start](#before-you-start)
- [Installation](#installation)
- [Each Time You Stream](#each-time-you-stream)
- [Using cordmux](#using-cordmux)
- [Troubleshooting](#troubleshooting)
- [Updating](#updating)
- [Uninstalling](#uninstalling)
- [Compatibility](#compatibility)
- [Source Code and Licenses](#source-code-and-licenses)

## Before You Start

### Requirements

- A Windows 11 PC (64-bit)
- The x64 version of OBS Studio 33 or later (the Arm64 version of OBS isn't supported). The steps in this guide show the screens of OBS 33.
- Vesktop, an app for using Discord that's separate from the official Discord app. You set it up in step 1 of "Installation". The portable version of Vesktop isn't supported.
- If you join calls in the official Discord app: a second Discord account. Creating it requires an email address other than your main account's.

Other people in the call don't need to install or set up anything.

### Choosing how to use cordmux

How you set up cordmux depends on whether you join calls in Vesktop or in the official Discord app, because cordmux receives screen shares and voices through the account that's in the call in Vesktop.

- If you join calls in Vesktop, you can use your main account (the account you talk with) as is. However, cordmux can't receive your main account's screen share.
- If you join calls in the official Discord app, set up an alt account (a second account), log in to Vesktop with it, and join the same call. Also do step 4 of "Installation".

### What cordmux changes, and safety

- The Vencord that Vesktop loads (Vencord is a system for adding plugins to Vesktop) is switched to the one in the cordmux zip file. When you uninstall cordmux, Vesktop's settings go back to how they were before you added cordmux. Notices in the dock call the cordmux built into this Vencord "cordmux in Vesktop".
- cordmux passes video and voice between OBS and Vesktop within the same PC, and cordmux can't protect them from other programs running on that PC. Don't add untrusted plugins to the same Vesktop.

## Installation

When you finish these steps, the cordmux dock is connected to Vesktop, and you can show the call's screen shares in OBS.

### 1. Install Vesktop

If you already use Vesktop, skip this step.

1. Download the Windows installer from the [official website](https://vesktop.dev/) and install Vesktop.
2. Log in with your Discord account. Vesktop shows your list of servers. If you use an alt account, you switch to it in step 4.

### 2. Put cordmux in the OBS plugins folder

If OBS is running, close it.

1. Extract the cordmux zip file you downloaded.
2. Type `C:\ProgramData\obs-studio\plugins` into the address bar of File Explorer, and press Enter. If this folder doesn't exist, type `C:\ProgramData` into the address bar to open that folder. Create a folder named "obs-studio" there, and then create a folder named "plugins" inside "obs-studio".
3. Move the "cordmux" folder from the extracted files into the "plugins" folder. If you're asked for administrator permission, select [Continue]. If you're told that files with the same names exist, replace them.

This step is done when `C:\ProgramData\obs-studio\plugins\cordmux\cordmux.dll` exists. If it doesn't, extract the zip file again and redo this step.

If you use the portable version of OBS, instead of 2, 3, and the check above, move the "cordmux" folder from the extracted files into the "plugins" folder in the OBS folder. If this folder doesn't exist, create it. If you're told that files with the same names exist, replace them. This step is done when `plugins\cordmux\cordmux.dll` exists in the OBS folder.

If you don't have administrator permission and can't move the files to `C:\ProgramData`, use the portable version of OBS and the portable steps above.

### 3. Add cordmux to Vesktop, and add the source to a scene

1. Start OBS. The cordmux dock appears. The first time, it appears as a separate window. Drag its title bar to dock it wherever you like in the OBS window. If it doesn't appear, select [cordmux] from the [Docks] menu in OBS. If [cordmux] isn't there, see "[The dock doesn't appear](#the-dock-doesnt-appear)" in Troubleshooting.
2. Quit Vesktop. While Vesktop is running, you can't select [Add to Vesktop]. With its default settings, Vesktop doesn't quit when you close its window. It keeps running in the notification area at the right end of the taskbar. Right-click the Vesktop icon in the notification area and select [Quit]. If you don't see the icon, select [^] in the notification area to show hidden icons.
3. On the [People] tab of the dock, select [Add to Vesktop]. Vesktop is set to load the Vencord in the cordmux zip file.
   - If the button can't be selected and "Can't add while Vesktop is running. Quit Vesktop first." appears below it, do 2 again.
   - If you use another Vencord in Vesktop, "Switch Vesktop to the Vencord bundled with cordmux?" appears. Read it, and then select [Switch].
4. Select the scene you stream with, and select [+] under "Sources". A window opens, so select "cordmux" in the list of source types. In a scene without the "cordmux" source, call screen shares and voices don't go out on the stream.
5. Leave the source name as it is, and select [Create New].
6. The cordmux Properties window opens. Select [OK] to close it. The notice "The cordmux source hasn't been added yet" goes away from the dock's [Status] tab. If it doesn't, see "[Nothing in the OBS preview](#nothing-in-the-obs-preview)" in Troubleshooting.
7. If you use "Desktop Audio" in OBS, or have a source that captures audio from Vesktop or Discord, call voices may be heard twice on stream. Change those sources as described in "[Call voices are heard twice](#call-voices-are-heard-twice)" in Troubleshooting.
8. Start Vesktop.

If the top line of the dock (this guide calls it the first line) starts with "Connected to Vesktop" or "In a call", you're connected. If it doesn't, see "[Not connected to Vesktop](#not-connected-to-vesktop)" in Troubleshooting.

### 4. Set up an alt account (only if you use one)

1. In Discord, create the alt account with an email address other than your main account's.
2. With your main account, create and copy an invite link to the server you use for the call.
3. In Vesktop, log out of your main account, and log in with the alt account. Select [+] in the server list, select [Join a Server], paste the invite link, and select [Join Server]. Depending on the server's settings, you may be asked to verify a phone number.
4. In the official Discord app logged in with your main account, join the same call as the alt account.
5. If your main account and the alt account are both in the call on the same PC, the call's sound also plays from Vesktop, and your voice is also sent to the call from the alt account. In that case, mute and deafen the alt account in Vesktop. Neither affects the screen shares and voices that cordmux receives.
6. In the dock, select [Change] in the "Main" row at the top, and then choose your main account.

This step is done when the "Main" row and the "Alt" row at the top of the dock show the account names and the call names. If the "Alt" row doesn't appear, see "[No Alt row in the dock](#no-alt-row-in-the-dock)" in Troubleshooting. If the dock shows "Your main account (name) isn't in this call", see "[The dock says your main account isn't in this call](#the-dock-says-your-main-account-isnt-in-this-call)" in Troubleshooting.

## Each Time You Stream

1. Start OBS and Vesktop, and join the call in Vesktop. If you use an alt account, join with the alt account in Vesktop, and join the same call with your main account in the official Discord app. Keep the alt account muted and deafened in Vesktop. The first line of the dock shows the number of people, such as "In a call (4 people)". If it doesn't, see "[Not in a call](#not-in-a-call)" in Troubleshooting.
2. In OBS, select the scene with the "cordmux" source. In the list on the [People] tab of the dock, select the check box for each person whose screen share you want in a slot. When they're sharing their screen, it appears in the OBS preview. It can take a few seconds after you select the check box. If it doesn't appear, see "[Nothing in the OBS preview](#nothing-in-the-obs-preview)" in Troubleshooting.
   - If the [Slot Assignment] box on the [Display] tab of the dock is set to "Automatic", people who start sharing are selected automatically, so you don't need to do this.
3. To put a person's screen share audio on stream too, select the speaker button in that person's row on the [People] tab to unmute it. Screen share audio goes back to muted each time sharing begins, so do this every time. The "cordmux" meter in the OBS "Audio Mixer" moves. If it doesn't, see "[No voice or screen share audio in OBS](#no-voice-or-screen-share-audio-in-obs)" in Troubleshooting.
4. To record each person's screen share and voice to separate files, select [Start Recording] in the dock. cordmux recording is separate from OBS recording, and it's off each time OBS starts, so do this every time you record. When the stream is over, select [Stop Recording]. To start recording automatically, and to check the recordings, see "[Recording with cordmux](#recording-with-cordmux)" in "Using cordmux".

## Using cordmux

To open cordmux Settings, select [cordmux Settings] from the [Tools] menu in OBS.

### Choosing who appears in slots

You choose who appears in slots by selecting their check boxes in the list on the dock's [People] tab.

- Select the button with three dots in a vertical line at the right end of a row on the [People] tab, and a menu opens. This guide calls this menu the row's menu.
- If the [Slot Assignment] box on the [Display] tab is set to "Automatic", people who start sharing are selected automatically. Even when it's "Manual" (the default), people with [Add Automatically] selected in the row's menu are selected automatically when they start sharing.
- When a person stops sharing or leaves the call, their check box is cleared right away. Everyone's check boxes are also cleared when the Vesktop account leaves the call. It isn't selected again automatically, so select it again after they resume sharing (except for people with [Add Automatically] and when the box is set to "Automatic").
- You can't select the check box of a person who isn't sharing.

### Volume and mute of voices and screen share audio

For each person, you can mute and set the volume of their voice and screen share audio.

- You mute them in that person's row on the dock's [People] tab. The microphone button mutes the voice, and the speaker button mutes the screen share audio.
- You set the volume with the two sliders for that person on the dock's [Audio] tab: the upper one is the voice, and the lower one is the screen share audio. Double-click a slider to reset it to 100%.
- The OBS "Audio Mixer" doesn't show a meter for each person. While you stream, adjust the volume on the [Audio] tab while watching the meter under each slider, which shows how loud each person is. While a person is muted or set to 0%, the meter shows the received loudness in gray, so you can spot someone who's talking and decide whether to unmute them.
- Your main account's voice is muted by default. Unmute it only when you want your own voice received from Discord on the stream. If you unmute it while also streaming an OBS mic, your voice sounds doubled.
- If you use an alt account, deafening your main account in Discord doesn't affect what cordmux receives.

### Name labels and the speaking glow

To change how the names shown in slots (name labels) look, use the [Display] tab of the dock. Changes show on the stream right away.

- The name label of a person who's speaking glows. cordmux uses the voice received from Discord to decide whether you're speaking. If you select an audio source, such as a mic, in the [Self Slot Audio Source] box on the same tab, cordmux uses the volume of that source instead. When you use only your main account, cordmux doesn't receive your own voice from Discord, so your name label doesn't glow unless you select a source in the [Self Slot Audio Source] box.
- Everyone in the call, including you (your main account), has a name label somewhere on the stream. People shown in a slot with a name label have it in that slot. Everyone else (people not sharing their screen, the other people while someone is shown full screen, a person whose name label is hidden by the [Hide when only one person] check box, people sharing their screen when the [Show] check box under "Participants Sharing Screens" is cleared, and so on) lines up vertically at the bottom right of the canvas by default.
- You change their placement and layout under "Participants Not Sharing Screens" on the [Display] tab. To not show them, clear the [Show] check box in the same place. To leave out specific people, open [Participant Settings] from the dock's menu and select their check box in the "Hide Label When Voice Only" column. Bots aren't shown by default.

### Showing your own screen in a slot

Your main account's row works like the other participants' rows. Select the check box to add your main account's video to a slot. [Add Automatically] in the row's menu and "Automatic" in the [Slot Assignment] box also work the same as for other participants.

Choose your main account's video in the [Video for Self Slot] box on the [Display] tab of the dock.

- "Discord screen share": your main account's screen share, received by the alt account, fills one of the four slots like other participants'. This is the default when you've set a main account.
- An OBS source or scene (such as your own screen or a capture card): the video you choose appears in a slot. This guide calls it the self slot.
  - It appears only while the check box in your main account's row is selected. If you stop sharing in Discord, it stays as long as the check box is selected.
  - Your main account's Discord screen share isn't added to a slot.
  - The check box stays as you left it the next time you start OBS.
  - You can't choose a scene that contains the "cordmux" source.
  - The audio of the source you select doesn't go out through cordmux. To put that audio on stream, capture it with a separate audio source.
- "(None)": the default when no main account is set. Your main account's video isn't added to a slot. Other participants' screen shares fill the four slots, from the top left.

The [Place first] check box below the [Video for Self Slot] box is selected by default and puts the video you choose in the top left slot regardless of the [Slot Order] box. When you clear it, the video is placed by the [Slot Order] box like other participants' screen shares.

If you don't use an alt account, cordmux can't receive your main account's screen share, so you can't select the check box in your main account's row until you choose an OBS source or scene. When other participants fill all four slots, you can't select the check box either.

### Showing one person full screen

You can temporarily show just one person full screen, from the people in slots and the people sharing their screen.

- Select [Show Full Screen] in the row's menu, and only that person's video fills the stream, and the first line of the dock shows "Full screen: (name)". To go back, select [Restore] on the first line.
- On the [Hotkeys] page of OBS [Settings], you can assign keys to "cordmux: Show Top Left Slot Full Screen" (Top Right, Bottom Left, and Bottom Right work the same way) and "cordmux: Restore from Full Screen". Pressing the key for the same slot again also goes back.
- If you show someone who isn't in a slot full screen, cordmux starts receiving their screen share then, so it can take a few seconds to appear.
- While one person is full screen, the other participants' voices and the cordmux recording don't stop.
- If that person stops sharing or leaves the call, it goes back automatically.

### Checking the look with placeholders

To check how the slots and name labels look without a call, you can show placeholders in all slots. Turn them on or off with the [Show] check box under "Placeholders" at the bottom of the [Display] tab of the dock. While they're shown, nothing from the people in the call appears on the stream, and the self slot isn't shown. During that time, the first line of the dock starts with "Showing placeholders".

### Incoming video and encoding settings

- To choose how cordmux in Vesktop encodes the video it sends to OBS, use the [Video Encoder] box and the [Tuning] box on the [Incoming Video] page of cordmux Settings. A changed setting applies to screen shares that start after the change. The defaults, "Hardware" and "Low Latency", are enough in most cases. "Software" uses more CPU. "High Quality" favors image quality over latency.
- The [People to Receive] box on the same page is "Only People in Slots" by default. Vesktop receives only the screen shares of people in slots, so it doesn't appear as a viewer on the screen shares of people who aren't in a slot. If you set the box to "Everyone Sharing Their Screen", Vesktop receives everyone's video before you add them to slots, so it appears right after you select the check box, but the load on the PC is higher.

### Dock notices and the Status tab

- The first line of the dock shows the current status or the headline of a notice. Select it to open the [Status] tab, which shows the full text of each notice with its buttons, the state of the cordmux recording, how video from each participant is arriving, and a history of events.
- A notice that affects the stream or the recording and has something you can do right away turns the first line red and stays until it's resolved. The exception is a problem that stops only the cordmux recording (such as running out of disk space or not being able to write to the save folder): it leaves the first line when you open the [Status] tab. Other notices that need action show on the first line until you open the [Status] tab, and after that they remain only in the tab's count.
- The video table on the [Status] tab shows how many times each person's video gaps out and the total time (a running total since the stream started), plus the share of late frames and the maximum delay (past 1 minute). The totals reset when you start streaming or select [Reset Gap Totals], and they're cleared when you quit OBS.
- If video isn't smooth, adjust the [Video Buffer] box on the same tab while watching the Late column. A larger value plays video more smoothly, but the delay before it appears on stream increases. Right after you change the value, video and screen share audio stop or skip once.

### When the alt account is in another call

- When the call name in the "Alt" row is in the warning color, the alt account is in a different call from the main account. Select [Move] to move the alt account to the main account's call. Select [Leave] to make the alt account leave its call. This stops everyone's video and voice.
- While you stream, selecting [Leave] changes the [Move] and [Leave] buttons into [Leave] and [Stay in Call]. To skip it, clear the [Confirm Leaving While Streaming] check box on the [Advanced] page of cordmux Settings.

### Recording with cordmux

cordmux recording saves each person's screen share and voice to separate files. It's separate from OBS recording.

1. In the dock, select [Start Recording]. The button changes to [Stop Recording] and shows the elapsed time. If the save folder can't be written to, the first line turns red and a notice appears on the [Status] tab. Change the save folder as the notice says, and then select [Start Recording] again.
2. When the stream is over, select [Stop Recording]. "Finalizing the recording..." appears under "Current Notices" on the [Status] tab.
3. When it disappears, open the save folder with [Open Folder] on the [Recording] page of cordmux Settings, and check the cordmux recordings. If they aren't there, or a notice appears on the [Status] tab, see "[No cordmux recordings](#no-cordmux-recordings)" in Troubleshooting.

About recording:

- cordmux recording is off each time OBS starts. If you select the [Automatically record when streaming] check box on the [Recording] page of cordmux Settings, recording starts when you start streaming and stops when you stop.
- By default, the screen shares kept in cordmux recordings are only those of people while they're in slots. If you set the [People to Receive] box to "Everyone Sharing Their Screen", everyone's are kept. Voices are recorded for everyone, whether or not they're in a slot. Your cordmux recordings have the same quality as the video cordmux delivers to OBS.
- In addition to the finalized files, cordmux recordings keep the original data. In the save folder, there's a folder named with the date and time the recording started, and the original data is in the "data" folder inside it. Together, the original data and the finalized files use about twice the space of the finalized files. Don't delete the "data" folder until finalizing is done. After that, you can delete it.
- If OBS crashes or the PC shuts down or freezes while cordmux is recording or finalizing, the recordings aren't finalized right away. What was recorded up to then isn't deleted. The next time you start OBS, the dock shows "Some recordings haven't been finalized". Select [Resume Finalizing] to finalize them. The same notice appears if finalizing hadn't finished when you closed OBS.
- If a person's screen share changed resolution, or they stopped sharing and started again, their recording may be rebuilt into one file (in the ProRes 422 HQ format). The same happens if Vesktop quits during the recording. Rebuilt files are large. Apple's published target is about 100 GB per hour at 1920x1080 and 30 FPS, and about twice that at 60 FPS. Finalizing them also takes time.

### Moving the OBS folder

- If you move the folder of the portable version of OBS somewhere else, start OBS before Vesktop. In the other order, Vesktop puts the official Vencord in the old place and can't connect to cordmux. In that case, start OBS and quit Vesktop. When the first line of the dock shows "Vesktop isn't running", start Vesktop. You can delete the folder created in the old place.
- To move cordmux between the installed OBS and the portable version of OBS, or to a different portable OBS, first uninstall cordmux in the current OBS, and then install it in the other OBS. The cordmux settings and participant settings aren't carried over.

## Troubleshooting

### The dock doesn't appear

If [cordmux] is in the [Docks] menu in OBS, select it to show the dock. If it isn't there, check the following.

- OBS version: check it in [About] in the [Help] menu of OBS. If it's older than 33, update OBS.
- Files: check that `C:\ProgramData\obs-studio\plugins\cordmux\cordmux.dll` exists. For the portable version of OBS, check the places you moved the files to in step 2 of "Installation".
  - If the file isn't there, or some files are missing, extract the zip file again and redo step 2 of "Installation".
  - The dock also doesn't appear if you renamed the "cordmux" folder, or moved the whole extracted folder instead of the "cordmux" folder inside it. Put the folder back so that cordmux.dll is at the location above.
  - Do the same if text in the dock or cordmux Settings appears as identifiers that start with "cordmux." instead of normal wording.
- "Plugin Load Error": if this appears when OBS starts and "cordmux" is in its list, check that OBS is the x64 version. The Arm64 version isn't supported. If it's the x64 version, extract the zip file again and redo step 2 of "Installation".
- Plugin Manager: if the cordmux check box is cleared in [Plugin Manager] in the [Tools] menu of OBS, select it and restart OBS.
- Safe Mode: the dock also doesn't appear when OBS was started in Safe Mode. See "[OBS shows a Safe Mode confirmation when it starts](#obs-shows-a-safe-mode-confirmation-when-it-starts)".

### The dock shows "Couldn't add cordmux to Vesktop"

The reason is shown below it. If it says what to do, do that.

### Not connected to Vesktop

What to do depends on the notice on the first line or the [Status] tab.

- "Vesktop isn't running": start Vesktop.
- A notice that starts with "Disconnected from Vesktop": start Vesktop if it isn't running. If it's running but doesn't reconnect, restart Vesktop.
- "Vesktop isn't set up for cordmux" or "Vesktop is set to load a Vencord other than the one bundled with cordmux": quit Vesktop, select [Add to Vesktop], and then start Vesktop.
- "Restart Vesktop": quit Vesktop, and then start it again.
- "Vesktop is running but not connected": restart Vesktop. If that doesn't help, restart OBS too.
- "Another OBS is running cordmux": close one of the OBS windows.
- "Can't accept connections from Vesktop": restart OBS.

### Not in a call

If the first line of the dock starts with "Connected to Vesktop" and doesn't show "In a call", join the call in Vesktop. If you use an alt account, check that Vesktop is logged in with the alt account. If it doesn't show "Connected to Vesktop", see "[Not connected to Vesktop](#not-connected-to-vesktop)".

### Nothing in the OBS preview

- Check that the scene selected in OBS has the "cordmux" source. If the dock shows "The cordmux source hasn't been added yet", select [+] under "Sources" and add "cordmux".
- Check that the person you want to show is sharing their screen.
- If the [Slot Assignment] box is set to "Manual" (the default), check that the person's check box is selected in the list on the dock's [People] tab. It can take a few seconds after you select the check box before the video appears.
- If the person's row shows "Waiting for video..." or "Not receiving video": Check whether the screen shares appear in Vesktop.
- If the person's row shows "Can't show video", see the notice on the dock's [Status] tab.

### The self slot doesn't appear

The self slot appears only while an OBS source or scene is selected in the [Video for Self Slot] box and the check box in your main account's row in the list on the dock's [People] tab is selected.

- When the box is "Discord screen share" or "(None)", the self slot isn't used. With "Discord screen share", your main account's screen share is added to a slot like other participants'. To show your own screen through OBS instead, choose the OBS source or scene that shows it.
- Starting a screen share in Discord doesn't always select the check box. It's selected only when the [Slot Assignment] box is "Automatic" or [Add Automatically] in the row's menu is selected.
- When other participants fill all four slots, you can't select the check box. Clear another participant's check box to free a slot.
- If the row shows "Video not found", the source or scene you chose can't be found. Choose one again in the [Video for Self Slot] box.
- While placeholders are shown, the self slot isn't shown.

### No voice or screen share audio in OBS

Check the following.

- The scene you stream with has the "cordmux" source
- "cordmux" isn't muted in the "Audio Mixer" of OBS
- The microphone button isn't muted in the row of the person whose voice is missing, on the dock's [People] tab (your main account's voice is muted by default)
- The person's voice slider on the dock's [Audio] tab isn't turned down

If the screen share audio is missing, also check the speaker button in the same row. Screen share audio goes back to muted each time sharing begins.

### Call voices are heard twice

If another OBS source also captures Vesktop or Discord audio, call voices are heard twice on stream. Send call voices to the stream only through the "cordmux" source. Muting the "cordmux" source also stops screen share audio, so change the other sources as follows. The dock shows the name of the source that's capturing the audio.

- Sources that capture only Vesktop or Discord audio (such as "Application Audio Capture (BETA)"): mute or remove them.
- "Desktop Audio": if it captures the device Vesktop or Discord plays its sound on, it includes call voices. In that case, on the [Audio] page of [Settings] in the [File] menu of OBS, set "Desktop Audio" to "Disabled", and capture the sound you want on stream, such as a game, with an "Application Audio Capture (BETA)" source.

### No cordmux recordings

Your cordmux recordings are saved in the folder in the [Save Folder] box on the [Recording] page of cordmux Settings. For each recording, a folder named with the date and time the recording started is created inside that folder. For example, when the box says `D:\hoge`, a folder such as `D:\hoge\2000-01-01 21-30-45` is created.

- When you haven't chosen a folder, it's the "cordmux" folder inside the folder set as "Recording Path" in OBS. Clear the box to go back to it.
- Recordings made before you changed the save folder stay in the previous folder, and their finalized files are saved there too.
- If the dock shows "Some recordings haven't been finalized", select [Resume Finalizing].
- If the dock shows "Some recordings couldn't be finalized", check the free space on the drive that contains the folder shown in the dock. Free up space if needed, and then select [Resume Finalizing].
- If the dock shows "Can't write to the save folder.", select [Change Save Folder], choose a folder that can be written to, and then select [Start Recording] again.

### No Alt row in the dock

The "Alt" row appears below the "Main" row only when you use an alt account. In 6 of step 4 of "Installation", select [Change] in the "Main" row and choose your main account. If you choose the account that Vesktop is logged in with, the "Alt" row doesn't appear. Also check that Vesktop is logged in with the alt account.

### The dock says your main account isn't in this call

Check that your main account is in the same call. If the alt account is in a different call, select [Move] in the "Alt" row. If your main account is in the call and the notice still appears, select [Change] in the "Main" row at the top of the dock, and then choose your main account again.

### The dock shows "The cordmux in Vesktop has been overwritten"

In the dock, select [Reinstall]. If "Restart Vesktop" appears next, restart Vesktop. Right-clicking the Vesktop icon in the notification area and selecting [Repair Vencord] leads to this message. Don't use [Repair Vencord].

### Vesktop's window stays black

When Discord is updated and the Vencord bundled with cordmux doesn't support the update, Vesktop's window can stay black. Follow these steps to set Vesktop back to how it was before cordmux was added.

1. Right-click the Vesktop icon in the notification area and select [Quit] to quit Vesktop.
2. On the [Status] tab of the dock, select [Remove from Vesktop].
3. Start Vesktop.

When you update cordmux, quit Vesktop and select [Add to Vesktop] again. Then start Vesktop. Don't use [Repair Vencord].

### Still the old version after updating

Check that you closed OBS before replacing the files and that you moved them to the same place as before, and then do the steps in "[Updating](#updating)" again.

### Uninstalling stopped partway

If "Couldn't finish uninstalling" appears, read the lists under "Couldn't check:", "Couldn't restore:", and "Couldn't delete:". What to do next is shown under each.

- Quit Vesktop before you uninstall again.
- If "Vesktop settings" is under "Couldn't restore:", uninstall again to restore them. Don't delete the cordmux files until Vesktop's settings are restored. If you delete them, Vesktop puts the official Vencord where they were.
- If the same thing appears when you uninstall again, start Vesktop and select [Reset Vesktop] in the menu of the Vesktop icon in the notification area. Vesktop's settings go back to their initial state, and so does the Vencord location. If Vesktop asks you to log in, log in again. If you used another Vencord before, select its location again in Vesktop's settings. After that, you can delete the cordmux files.

### You deleted the cordmux folder before uninstalling

Extract the zip file again and put the "cordmux" folder in the same place as in step 2 of "Installation", and then do the steps in "[Uninstalling](#uninstalling)" again.

### [Uninstall cordmux] isn't in the [Tools] menu

If the cordmux check box is cleared in [Plugin Manager] in the [Tools] menu of OBS, cordmux isn't loaded and can't be uninstalled. Select the check box and restart OBS, and then uninstall. Clearing the check box alone doesn't restore the Vesktop settings. Don't delete the cordmux files until uninstalling is finished. Otherwise, see "[The dock doesn't appear](#the-dock-doesnt-appear)".

### OBS shows a Safe Mode confirmation when it starts

This appears when OBS didn't shut down properly last time. If you select [Run in Safe Mode], cordmux isn't loaded and the dock doesn't appear. To use cordmux, select [Run in Normal Mode]. If OBS started in Safe Mode, select [Restart in Normal Mode] from the [Help] menu of OBS.

### If nothing else works

Select [Export Diagnostics] on the [About] page of cordmux Settings to save diagnostic information in one zip file. You can use it to look into the cause yourself or to get help from someone familiar with it.

- It contains the cordmux settings, the cordmux log, the log of cordmux in Vesktop, and versions. The OBS log isn't included. If OBS and Vesktop have never connected, the log of cordmux in Vesktop isn't included.
- Discord names and participant settings aren't included either. Discord user IDs are replaced with other numbers, and your PC user name and the token used for the connection between OBS and Vesktop are hidden.

## Updating

Check for new versions on the [release page](https://github.com/eoforgu0/cordmux/releases). cordmux doesn't check whether a new version is available.

1. Close OBS, extract the new zip file, and move the "cordmux" folder in it to the same place as before. Replace the files that have the same names.
2. Start OBS, and select [cordmux Settings] from the [Tools] menu. On the [About] page, check that "cordmux (OBS)" shows the new version. If it still shows the old version, see "[Still the old version after updating](#still-the-old-version-after-updating)" in Troubleshooting.
3. If the dock shows "The cordmux version in Vesktop doesn't match the one in OBS", restart Vesktop. The first line of the dock starts with "Connected to Vesktop" or "In a call". If it doesn't, see "[Not connected to Vesktop](#not-connected-to-vesktop)" in Troubleshooting.

To go back to a previous version, do the same steps with the zip file of that version.

## Uninstalling

Don't delete the cordmux files until uninstalling is finished. If you delete them first, Vesktop puts the official Vencord where they were. If you did, see "[You deleted the cordmux folder before uninstalling](#you-deleted-the-cordmux-folder-before-uninstalling)" in Troubleshooting.

1. Stop streaming, OBS recording, the replay buffer, the virtual camera, and cordmux recording, and quit Vesktop. Closing the Vesktop window doesn't quit it. Right-click the Vesktop icon in the notification area and select [Quit]. You can't uninstall while any of them is running.
2. In OBS, select [Uninstall cordmux] from the [Tools] menu. "Uninstall cordmux?" appears. Read what will be deleted and what stays, and then select [Uninstall]. The "cordmux" source is deleted from all scene collections in OBS. If this confirmation doesn't appear, the [About] page opens. The reason you can't uninstall is shown below the [Uninstall cordmux] button.
3. "Close OBS, then delete the cordmux folder" appears. Note the location under "Folder to delete:" or select [Open Folder Location]. Then close OBS, and delete that folder. If you created folders in step 2 of "Installation", also delete those folders once they're empty. If "Couldn't finish uninstalling" appears, see "[Uninstalling stopped partway](#uninstalling-stopped-partway)" in Troubleshooting.

## Compatibility

cordmux may stop working when OBS, Vesktop, or Discord is updated.

## Source Code and Licenses

The code of cordmux itself is published under Apache-2.0 (Apache License 2.0). The cordmux zip file contains GPL components, such as Vencord and FFmpeg (a GPL version 3 or later build). The corresponding source code for the whole zip file is therefore published as those licenses require. The source code is available from the places listed in "[ソースコードの入手先.txt](../dist/ソースコードの入手先.txt)" and from the files attached to the release of each version.

<!-- zip:source_url -->

The license of each component and where to get its source are listed in the file "ソースコードの入手先.txt" (its first line says "Where to get the source code", and it also has English lines). The license texts are in the "[licenses](../dist/licenses)" folder in the zip file.
