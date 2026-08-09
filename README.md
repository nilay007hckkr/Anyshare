<div align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=gradient&customColorList=0984E3,00B894&height=200&section=header&text=AnyShare&fontSize=80&fontColor=ffffff" width="100%" />

  <br />
  
  <a href="https://github.com/nilay007hckkr/Anyshare">
    <img src="https://readme-typing-svg.herokuapp.com?font=Fira+Code&weight=700&size=28&pause=1000&color=2D3436&center=true&vCenter=true&width=800&lines=High-Speed+Cross-Platform+Network+Engine;Blazing+Fast+TCP+Socket+Transfers;Seamless+UI+%26+Handover+Decoupling" alt="Typing Subtitle" />
  </a>

  <br />
  
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white" alt="C++17"/>
  <img src="https://img.shields.io/badge/CMake-3.10%2B-064F8C?style=for-the-badge&logo=cmake&logoColor=white" alt="CMake"/>
  <img src="https://img.shields.io/badge/Boost.Asio-1.74%2B-F3A622?style=for-the-badge&logo=boost&logoColor=white" alt="Boost"/>

</div>

<br />

<table align="center" width="100%">
  <tr>
    <td align="center" width="50%">
      <h3>Network Engine</h3>
      <p>Utilizes asynchronous TCP sockets via Boost.Asio for non-blocking, lightning-fast 64KB chunk streaming.</p>
    </td>
    <td align="center" width="50%">
      <h3>Polymorphic Handover</h3>
      <p>Injectable strategy pattern allows seamless connection discovery via NFC, QR Codes, or Bluetooth.</p>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <h3>UI Agnostic</h3>
      <p>Observer pattern exposes real-time progress updates without locking into any specific graphical framework.</p>
    </td>
    <td align="center" width="50%">
      <h3>Cross Platform</h3>
      <p>Strict C++17 compliance guarantees flawless compilation on Android JNI, Linux, and Windows Desktop.</p>
    </td>
  </tr>
</table>

<br />
<div align="center"><img src="https://capsule-render.vercel.app/api?type=rect&color=00B894&height=4&width=300" /></div>
<br />

## Core Architecture

The system is built on a highly modular architecture. The core `NetworkEngine` manages all asynchronous data streams while delegating discovery and progress reporting to injected interfaces.

```mermaid
graph TD
    classDef core fill:#2d3436,stroke:#0984e3,stroke-width:2px,color:#fff
    classDef interface fill:#0984e3,stroke:#00b894,stroke-width:2px,color:#fff
    classDef external fill:#00b894,stroke:#2d3436,stroke-width:2px,color:#fff

    UI["Platform UI (Android / Desktop)"]:::external
    Observer["IProgressObserver (Interface)"]:::interface
    Handover["INfcHandover (Interface)"]:::interface
    Engine["NetworkEngine (Core)"]:::core
    Asio["Boost.Asio Subsystem"]:::core

    UI -->|Implements| Observer
    UI -->|Uses| Handover
    Observer -.->|Attached to| Engine
    Handover -.->|Injected into| Engine
    Engine ==>|Manages| Asio
```

<br />
<div align="center"><img src="https://capsule-render.vercel.app/api?type=rect&color=0984E3&height=4&width=300" /></div>
<br />

## Dynamic Transfer Workflow

The following sequence details the lifecycle of a file transfer session, from the initial hardware-level handshake to the completion of the binary stream.

```mermaid
sequenceDiagram
    participant UI as User Interface
    participant Handover as NFC / Handover Layer
    participant Engine as NetworkEngine
    participant Remote as Remote Client

    UI->>Handover: Initiate Discovery
    Handover->>Remote: Out-of-band Handshake
    Remote-->>Handover: ConnectionInfo (IP, Port, Size)
    Handover-->>UI: Handover Complete
    
    rect rgb(45, 52, 54)
    Note over UI,Engine: Transfer Initialization
    UI->>Engine: startTransfer(HandoverStrategy)
    end
    
    rect rgb(9, 132, 227)
    Note over Engine,Remote: Asynchronous Streaming Phase
    Engine->>Remote: TCP async_connect()
    loop For each 64KB chunk
        Engine->>Remote: async_write(buffer)
        Engine->>UI: onProgress(percent)
    end
    Engine->>Remote: Graceful Socket Shutdown
    end
    
    Engine->>UI: onProgress(100)
```

<br />
<div align="center"><img src="https://capsule-render.vercel.app/api?type=rect&color=00B894&height=4&width=300" /></div>
<br />

## Build and Setup Instructions

### Prerequisites
- **CMake** (version 3.10 or higher)
- **C++17 Compiler** (GCC, Clang, or MSVC)
- **Boost Libraries** (version 1.74 or higher, specifically `Boost::system`)
- **Threads** (Standard threading library)

### Compilation

The project uses CMake to build a static library (`anyshare_engine`) which can be linked into your respective frontend or desktop application.

1. **Clone the repository:**
   ```bash
   git clone https://github.com/nilay007hckkr/Anyshare.git
   cd Anyshare
   ```

2. **Configure the project:**
   ```bash
   mkdir build
   cd build
   cmake ..
   ```

3. **Build the engine:**
   ```bash
   cmake --build .
   ```

### Application Integration

To use the AnyShare engine in your own application, add the following to your target's `CMakeLists.txt`:

```cmake
add_subdirectory(path/to/Anyshare)
target_link_libraries(your_target PRIVATE anyshare_engine)
```

<br />
<div align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=gradient&customColorList=00B894,0984E3&height=100&section=footer" width="100%" />
</div>

<div align="center">
  <h2>Project Contributors</h2>
  <table align="center" width="80%">
    <tr>
      <td align="center" width="33%">
        <strong>Nilay Joshi</strong><br />
        <i>Core Network & Systems Engineer</i>
      </td>
      <td align="center" width="33%">
        <strong>Mayank Joshi</strong><br />
        <i>Desktop NFC & Hardware Specialist</i>
      </td>
      <td align="center" width="33%">
        <strong>Mohd Almas khan</strong><br />
        <i>Android Mobile & JNI Engineer</i>
      </td>
    </tr>
  </table>
</div>
