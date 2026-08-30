// CI for the PC Monitor 2004 LCD firmware (Arduino Nano, AVR).
//
// Runs on the self-hosted Jenkins controller. No secrets header and no unit
// tests in this project, so the pipeline's job is to prove all three board
// targets still compile — and, more usefully, to keep an eye on how close the
// ATmega168P build is to running out of room.
//
// platformio.ini sets default_envs = nanoatmega168, so a bare `pio run` would
// build only that one. Each board is built explicitly, in its own stage, so a
// failure names the board rather than just "the build".
//
// The 168P is the tight one: 16 KB flash and 1 KB SRAM, which is why
// platformio.ini shrinks SERIAL_TX_BUFFER_SIZE to 16. SRAM is the real
// constraint on AVR — an image that links fine can still fail at runtime once
// the stack collides with the heap, and PlatformIO's "RAM: x%" line is the
// early warning. It is printed for every board below.

pipeline {
  agent any

  options {
    timestamps()
    timeout(time: 20, unit: 'MINUTES')
    disableConcurrentBuilds()
    buildDiscarder(logRotator(numToKeepStr: '20', artifactNumToKeepStr: '10'))
  }

  environment {
    // Shared with the other PlatformIO jobs; the AVR toolchain is small but the
    // cache also holds the ESP32 one, so it stays outside the workspace.
    PLATFORMIO_CORE_DIR = "${JENKINS_HOME}/.platformio"
  }

  stages {
    stage('Checkout') {
      steps {
        checkout scm
        sh 'git --no-pager log -1 --oneline'
      }
    }

    stage('Build ATmega168P (default, tightest)') {
      steps {
        sh 'pio run -e nanoatmega168'
      }
    }

    stage('Build ATmega328P') {
      steps {
        sh 'pio run -e nanoatmega328'
      }
    }

    stage('Build ATmega328P (optiboot)') {
      steps {
        sh 'pio run -e nanoatmega328new'
      }
    }

    stage('Archive hex images') {
      steps {
        sh 'ls -lh .pio/build/*/firmware.hex'
        archiveArtifacts artifacts: '.pio/build/*/firmware.hex,.pio/build/*/firmware.elf',
                         fingerprint: true,
                         onlyIfSuccessful: true
      }
    }

    // Publishing happens ONLY on a tag build. Multibranch creates a separate job
    // per discovered tag and sets TAG_NAME there, so buildingTag() is exact -
    // unlike checking whether HEAD carries a tag, which also fires on the main
    // job whenever main's head is the tagged commit.
    stage('Publish GitHub Release') {
      when { buildingTag() }
      steps {
        withCredentials([string(credentialsId: 'github-pat', variable: 'GH_TOKEN')]) {
          sh '''
            set -eu
            # one hex per board - all three are published so a flash can pick
            # the right Nano variant without rebuilding
            for e in nanoatmega168 nanoatmega328 nanoatmega328new; do
              cp ".pio/build/$e/firmware.hex" "pc-monitor-${e}-${TAG_NAME}.hex"
            done
            python3 scripts/publish_release.py \
              --repo Rozakos/Arduino-PC-Monitor-2004 \
              --tag "${TAG_NAME}" \
              --asset "pc-monitor-nanoatmega168-${TAG_NAME}.hex" \
              --asset "pc-monitor-nanoatmega328-${TAG_NAME}.hex" \
              --asset "pc-monitor-nanoatmega328new-${TAG_NAME}.hex"
          '''
        }
      }
    }
  }

  post {
    always {
      // One line per board so flash/SRAM pressure is visible at a glance and
      // comparable build to build.
      sh '''
        for d in .pio/build/*/; do
          env_name=$(basename "$d")
          if [ -f "$d/firmware.hex" ]; then
            size=$(stat -c%s "$d/firmware.hex")
            echo "${env_name}: firmware.hex = ${size} bytes - see the RAM:/Flash: lines above for headroom"
          fi
        done
      '''
    }
    cleanup {
      cleanWs()
    }
  }
}
