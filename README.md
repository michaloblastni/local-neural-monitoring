# Local Neural Monitoring 0.2.0

**Local Neural Monitoring** is a simple Windows application that monitors and records your neural activity at home. It works with the [Olimex EEG-SMT device](https://www.olimex.com/Products/EEG/OpenEEG/), which you plug into your computer with a USB cable. The application shows two channels with your neural activity, allows you to filter the alpha waves, beta waves, and other. You can also record your neural activity with it and later analyze it using Jupyter notebook to investigate something that you want to know.

![Local Neural Monitoring](local_neural_monitoring.png)

## 🗃️ Releases
- [0.2.0](https://github.com/michaloblastni/local-neural-monitoring/releases/tag/0.2.0)
- [0.1.1](https://github.com/michaloblastni/local-neural-monitoring/releases/tag/0.1.1)
- [0.0.1](https://github.com/michaloblastni/local-neural-monitoring/releases/tag/0.0.1)

## ✅ What it does

- 📡 Neural monitoring from Olimex EEG-SMT (CH1 and CH2)
- 📊 Lets you focus on specific brainwave types (Alpha, Beta, Gamma, Delta, Theta)
- 💾 Can save your neural activity to a file (CSV format)
- 🧾 Help menu for complete beginners who are new to neural monitoring
- ⚙️ Direct serial communication at 57600 baud
- 🧰 Startup check to automatically fix the crazy mouse issue
- 📉 Double-buffered chart rendering using GDI for smooth plotting
- 🧠 Visual distinction between CH1 and CH2 with labeled axes
- 🧼 Handles dropped packets and reports counter gaps


## 🧪 Requirements

- A Windows 10 or 11 computer
- The EEG device and accessories:
- [Olimex EEG-SMT device](https://www.olimex.com/Products/EEG/OpenEEG/EEG-SMT/open-source-hardware)
- 4x Active Sensor [EEG-AE](https://www.olimex.com/Products/EEG/Electrodes/EEG-AE/open-source-hardware)
- 1x Passive Sensor [EEG-PE](https://www.olimex.com/Products/EEG/Electrodes/EEG-PE/open-source-hardware)
- 1x Usb Cable [CABLE-USB-A-B-1.8M](https://www.olimex.com/Products/Components/Cables/USB/CABLE-USB-A-B-1.8M/)
- A small driver so your computer can recognize the device (Windows Universal): [ftdichip.com](https://www.ftdichip.com/Drivers/VCP.htm)
Total hardware cost: **143 EUR**
  
## 🧭 How to start (even if you're not a developer)

1. Download the .exe from the latest release
2. Plug the device into a USB port
3. Run the application and you will see your EEG waveform updated in real time
4. See Help → Contents to learn the basics
6. Filter brainwave type using the Band menu if you want to investigate some specific neural activity
7. Press File → Start recording / stop recording if you want to record your neural activity into a file

Once you become proficient by reading the Help and experimenting with the device, you can use it also with more advanced software such as Electric Guru, OpenViBE, BrainBay.

## 📂 File Overview for developers
- `Makefile` - Makefile for MINGW
- `local_neural_monitoring.c` – Core application code (UI, serial I/O, band filtering, plotting)
- `serial.c` - stopping serial
- `recording.c` - EEG data recording (CSV file format)
- `help.chm` *(optional)* – Local help file, opened from the Help menu

## 📜 License

MIT License © 2025 Michal Oblastni

This project is experimental and not intended for medical use.
