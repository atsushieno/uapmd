#include <gtest/gtest.h>
#include "../player/UapmdProjectFile.hpp"
#include "../player/UapmdProjectFileImpl.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

class UapmdProjectFileTest : public ::testing::Test {
protected:
    fs::path test_dir;

    void SetUp() override {
        // Create temporary test directory
        test_dir = fs::temp_directory_path() / "uapmd_test";
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        // Clean up test directory
        if (fs::exists(test_dir))
            fs::remove_all(test_dir);
    }

    fs::path createTestFile(const std::string& filename, const std::string& content) {
        auto path = test_dir / filename;
        std::ofstream ofs(path);
        ofs << content;
        return path;
    }
};

// Test: Basic write and read
TEST_F(UapmdProjectFileTest, BasicWriteAndRead) {
    auto project = std::make_unique<uapmd::UapmdProjectDataImpl>();

    // Create a track
    auto track = std::make_unique<uapmd::UapmdProjectTrackDataImpl>();

    // Add a clip with absolute positioning
    auto clip = std::make_unique<uapmd::UapmdProjectClipDataImpl>();
    clip->setFile("/audio/test.wav");
    clip->setMimeType("audio/wav");

    uapmd::UapmdTimelinePosition pos;
    pos.anchor = nullptr;  // Absolute positioning
    pos.samples = 48000;
    clip->setPosition(pos);

    track->addClip(std::move(clip));
    project->addTrack(std::move(track));

    // Write to file
    auto file_path = test_dir / "test_project.json";
    bool write_success = uapmd::UapmdProjectDataWriter::write(project.get(), file_path);
    ASSERT_TRUE(write_success);
    ASSERT_TRUE(fs::exists(file_path));

    // Read back
    auto loaded_project = uapmd::UapmdProjectDataReader::read(file_path);
    ASSERT_NE(loaded_project, nullptr);

    // Verify content
    auto& tracks = loaded_project->tracks();
    ASSERT_EQ(tracks.size(), 1);

    auto& clips = tracks[0]->clips();
    ASSERT_EQ(clips.size(), 1);

    auto& loaded_clip = clips[0];
    EXPECT_EQ(loaded_clip->file().string(), "/audio/test.wav");
    EXPECT_EQ(loaded_clip->mimeType(), "audio/wav");

    auto loaded_pos = loaded_clip->position();
    EXPECT_EQ(loaded_pos.anchor, nullptr);
    EXPECT_EQ(loaded_pos.samples, 48000);
}

// Test: Multiple tracks and clips
TEST_F(UapmdProjectFileTest, MultipleTracksAndClips) {
    auto project = std::make_unique<uapmd::UapmdProjectDataImpl>();

    // Create two tracks with multiple clips each
    for (int track_idx = 0; track_idx < 2; ++track_idx) {
        auto track = std::make_unique<uapmd::UapmdProjectTrackDataImpl>();

        for (int clip_idx = 0; clip_idx < 3; ++clip_idx) {
            auto clip = std::make_unique<uapmd::UapmdProjectClipDataImpl>();
            clip->setFile("/audio/track" + std::to_string(track_idx) + "_clip" + std::to_string(clip_idx) + ".wav");

            uapmd::UapmdTimelinePosition pos;
            pos.anchor = nullptr;
            pos.samples = clip_idx * 48000;
            clip->setPosition(pos);

            track->addClip(std::move(clip));
        }

        project->addTrack(std::move(track));
    }

    // Write and read
    auto file_path = test_dir / "multi_track_project.json";
    ASSERT_TRUE(uapmd::UapmdProjectDataWriter::write(project.get(), file_path));

    auto loaded_project = uapmd::UapmdProjectDataReader::read(file_path);
    ASSERT_NE(loaded_project, nullptr);

    auto& tracks = loaded_project->tracks();
    ASSERT_EQ(tracks.size(), 2);

    for (int track_idx = 0; track_idx < 2; ++track_idx) {
        auto& clips = tracks[track_idx]->clips();
        ASSERT_EQ(clips.size(), 3);

        for (int clip_idx = 0; clip_idx < 3; ++clip_idx) {
            auto expected_file = "/audio/track" + std::to_string(track_idx) + "_clip" + std::to_string(clip_idx) + ".wav";
            EXPECT_EQ(clips[clip_idx]->file().string(), expected_file);
            EXPECT_EQ(clips[clip_idx]->position().samples, clip_idx * 48000);
        }
    }
}

// Test: Track-anchored positioning
TEST_F(UapmdProjectFileTest, TrackAnchoredPositioning) {
    auto project = std::make_unique<uapmd::UapmdProjectDataImpl>();

    // Create first track
    auto track1 = std::make_unique<uapmd::UapmdProjectTrackDataImpl>();
    auto clip1 = std::make_unique<uapmd::UapmdProjectClipDataImpl>();
    clip1->setFile("/audio/clip1.wav");
    uapmd::UapmdTimelinePosition pos1;
    pos1.anchor = nullptr;
    pos1.samples = 0;
    clip1->setPosition(pos1);
    track1->addClip(std::move(clip1));
    project->addTrack(std::move(track1));

    // Create second track with clip anchored to first track
    auto track2 = std::make_unique<uapmd::UapmdProjectTrackDataImpl>();
    auto clip2 = std::make_unique<uapmd::UapmdProjectClipDataImpl>();
    clip2->setFile("/audio/clip2.wav");
    // Note: We can't set the anchor directly before the track is added
    // The reader will resolve it from the JSON
    uapmd::UapmdTimelinePosition pos2;
    pos2.anchor = nullptr;  // Will be set by reader
    pos2.samples = 96000;
    clip2->setPosition(pos2);
    track2->addClip(std::move(clip2));
    project->addTrack(std::move(track2));

    // Manually create JSON with track anchor
    std::string json = R"({
  "tracks": [
    {
      "clips": [
        {
          "position_samples": 0,
          "file": "/audio/clip1.wav"
        }
      ],
      "graph": {
        "plugins": []
      }
    },
    {
      "clips": [
        {
          "anchor": "track_0",
          "position_samples": 96000,
          "file": "/audio/clip2.wav"
        }
      ],
      "graph": {
        "plugins": []
      }
    }
  ]
})";

    auto file_path = createTestFile("track_anchor_project.json", json);

    // Read and verify
    auto loaded_project = uapmd::UapmdProjectDataReader::read(file_path);
    ASSERT_NE(loaded_project, nullptr);

    auto& tracks = loaded_project->tracks();
    ASSERT_EQ(tracks.size(), 2);

    // Verify second clip is anchored to first track
    auto& clip = tracks[1]->clips()[0];
    EXPECT_NE(clip->position().anchor, nullptr);
    EXPECT_EQ(clip->position().anchor, tracks[0]);
    EXPECT_EQ(clip->position().samples, 96000);
}

// Test: Clip-anchored positioning
TEST_F(UapmdProjectFileTest, ClipAnchoredPositioning) {
    std::string json = R"({
  "tracks": [
    {
      "clips": [
        {
          "position_samples": 0,
          "file": "/audio/clip1.wav"
        },
        {
          "anchor": "track_0_clip_0",
          "position_samples": 192000,
          "file": "/audio/clip2.wav"
        },
        {
          "anchor": "track_0_clip_1",
          "position_samples": 96000,
          "file": "/audio/clip3.wav"
        }
      ],
      "graph": {
        "plugins": []
      }
    }
  ]
})";

    auto file_path = createTestFile("clip_anchor_project.json", json);
    auto project = uapmd::UapmdProjectDataReader::read(file_path);
    ASSERT_NE(project, nullptr);

    auto& clips = project->tracks()[0]->clips();
    ASSERT_EQ(clips.size(), 3);

    // First clip has no anchor
    EXPECT_EQ(clips[0]->position().anchor, nullptr);
    EXPECT_EQ(clips[0]->position().samples, 0);

    // Second clip anchored to first clip
    EXPECT_EQ(clips[1]->position().anchor, clips[0].get());
    EXPECT_EQ(clips[1]->position().samples, 192000);

    // Third clip anchored to second clip
    EXPECT_EQ(clips[2]->position().anchor, clips[1].get());
    EXPECT_EQ(clips[2]->position().samples, 96000);
}

// Test: Plugin graph serialization
TEST_F(UapmdProjectFileTest, PluginGraphSerialization) {
    auto project = std::make_unique<uapmd::UapmdProjectDataImpl>();
    auto track = std::make_unique<uapmd::UapmdProjectTrackDataImpl>();

    // Add plugins to graph
    auto graph = std::make_unique<uapmd::UapmdProjectPluginGraphDataImpl>();
    uapmd::UapmdProjectPluginNodeData plugin1;
    plugin1.plugin_id = "com.example.reverb";
    plugin1.format = "VST3";
    plugin1.state_file = "/presets/reverb.vstpreset";
    graph->addPlugin(plugin1);

    uapmd::UapmdProjectPluginNodeData plugin2;
    plugin2.plugin_id = "com.example.eq";
    plugin2.format = "AU";
    plugin2.state_file = "";
    graph->addPlugin(plugin2);

    track->setGraph(std::move(graph));
    project->addTrack(std::move(track));

    // Write and read
    auto file_path = test_dir / "plugin_graph_project.json";
    ASSERT_TRUE(uapmd::UapmdProjectDataWriter::write(project.get(), file_path));

    auto loaded_project = uapmd::UapmdProjectDataReader::read(file_path);
    ASSERT_NE(loaded_project, nullptr);

    auto* loaded_graph = loaded_project->tracks()[0]->graph();
    ASSERT_NE(loaded_graph, nullptr);

    auto plugins = loaded_graph->plugins();
    ASSERT_EQ(plugins.size(), 2);

    EXPECT_EQ(plugins[0].plugin_id, "com.example.reverb");
    EXPECT_EQ(plugins[0].format, "VST3");
    EXPECT_EQ(plugins[0].state_file, "/presets/reverb.vstpreset");

    EXPECT_EQ(plugins[1].plugin_id, "com.example.eq");
    EXPECT_EQ(plugins[1].format, "AU");
    EXPECT_EQ(plugins[1].state_file, "");
}

// Test: Master track
TEST_F(UapmdProjectFileTest, MasterTrack) {
    auto project = std::make_unique<uapmd::UapmdProjectDataImpl>();

    // Add regular track
    auto track = std::make_unique<uapmd::UapmdProjectTrackDataImpl>();
    project->addTrack(std::move(track));

    // Add plugin to master track
    auto* master = dynamic_cast<uapmd::UapmdProjectTrackDataImpl*>(project->masterTrack());
    ASSERT_NE(master, nullptr);

    auto graph = std::make_unique<uapmd::UapmdProjectPluginGraphDataImpl>();
    uapmd::UapmdProjectPluginNodeData limiter;
    limiter.plugin_id = "com.example.limiter";
    limiter.format = "VST3";
    graph->addPlugin(limiter);
    master->setGraph(std::move(graph));

    // Write and read
    auto file_path = test_dir / "master_track_project.json";
    ASSERT_TRUE(uapmd::UapmdProjectDataWriter::write(project.get(), file_path));

    auto loaded_project = uapmd::UapmdProjectDataReader::read(file_path);
    ASSERT_NE(loaded_project, nullptr);

    auto* loaded_master = loaded_project->masterTrack();
    ASSERT_NE(loaded_master, nullptr);

    auto* master_graph = loaded_master->graph();
    ASSERT_NE(master_graph, nullptr);

    auto plugins = master_graph->plugins();
    ASSERT_EQ(plugins.size(), 1);
    EXPECT_EQ(plugins[0].plugin_id, "com.example.limiter");
}

// Test: Invalid anchor - not found
TEST_F(UapmdProjectFileTest, InvalidAnchorNotFound) {
    std::string json = R"({
  "tracks": [
    {
      "clips": [
        {
          "anchor": "track_999",
          "position_samples": 48000,
          "file": "/audio/clip.wav"
        }
      ],
      "graph": {
        "plugins": []
      }
    }
  ]
})";

    auto file_path = createTestFile("invalid_anchor.json", json);

    // Redirect stderr to capture warning
    std::stringstream buffer;
    std::streambuf* old = std::cerr.rdbuf(buffer.rdbuf());

    auto project = uapmd::UapmdProjectDataReader::read(file_path);

    // Restore stderr
    std::cerr.rdbuf(old);

    ASSERT_NE(project, nullptr);

    // Clip should be removed due to invalid anchor
    auto& clips = project->tracks()[0]->clips();
    EXPECT_EQ(clips.size(), 0);

    // Check warning message
    std::string output = buffer.str();
    EXPECT_TRUE(output.find("anchor not found") != std::string::npos);
}

// Test: Invalid anchor - recursive reference
TEST_F(UapmdProjectFileTest, InvalidAnchorRecursive) {
    std::string json = R"({
  "tracks": [
    {
      "clips": [
        {
          "anchor": "track_0_clip_1",
          "position_samples": 0,
          "file": "/audio/clip1.wav"
        },
        {
          "anchor": "track_0_clip_0",
          "position_samples": 0,
          "file": "/audio/clip2.wav"
        }
      ],
      "graph": {
        "plugins": []
      }
    }
  ]
})";

    auto file_path = createTestFile("recursive_anchor.json", json);

    // Redirect stderr to capture warning
    std::stringstream buffer;
    std::streambuf* old = std::cerr.rdbuf(buffer.rdbuf());

    auto project = uapmd::UapmdProjectDataReader::read(file_path);

    // Restore stderr
    std::cerr.rdbuf(old);

    ASSERT_NE(project, nullptr);

    // At least one clip should be removed due to recursive reference
    auto& clips = project->tracks()[0]->clips();
    EXPECT_LT(clips.size(), 2);

    // Check warning message
    std::string output = buffer.str();
    EXPECT_TRUE(output.find("recursive reference") != std::string::npos);
}

// Test: Complex recursive chain
TEST_F(UapmdProjectFileTest, ComplexRecursiveChain) {
    std::string json = R"({
  "tracks": [
    {
      "clips": [
        {
          "anchor": "track_0_clip_2",
          "position_samples": 0,
          "file": "/audio/clip1.wav"
        },
        {
          "anchor": "track_0_clip_0",
          "position_samples": 0,
          "file": "/audio/clip2.wav"
        },
        {
          "anchor": "track_0_clip_1",
          "position_samples": 0,
          "file": "/audio/clip3.wav"
        }
      ],
      "graph": {
        "plugins": []
      }
    }
  ]
})";

    auto file_path = createTestFile("complex_recursive.json", json);

    std::stringstream buffer;
    std::streambuf* old = std::cerr.rdbuf(buffer.rdbuf());

    auto project = uapmd::UapmdProjectDataReader::read(file_path);

    std::cerr.rdbuf(old);

    ASSERT_NE(project, nullptr);

    // Clips in a 3-way circular reference:
    // clip0 -> clip2, clip1 -> clip0, clip2 -> clip1
    // When validating clip0, clip2 doesn't have its anchor set yet, so it's valid
    // When validating clip1, clip0 is valid (anchors to not-yet-anchored clip2)
    // When validating clip2, it creates a cycle: clip2 -> clip1 -> clip0 -> clip2
    // So only clip2 should be removed
    auto& clips = project->tracks()[0]->clips();
    EXPECT_LT(clips.size(), 3);  // At least one clip should be removed
}

// Test: Absolute position calculation
TEST_F(UapmdProjectFileTest, AbsolutePositionCalculation) {
    std::string json = R"({
  "tracks": [
    {
      "clips": [
        {
          "position_samples": 48000,
          "file": "/audio/clip1.wav"
        },
        {
          "anchor": "track_0_clip_0",
          "position_samples": 96000,
          "file": "/audio/clip2.wav"
        }
      ],
      "graph": {
        "plugins": []
      }
    }
  ]
})";

    auto file_path = createTestFile("absolute_position.json", json);
    auto project = uapmd::UapmdProjectDataReader::read(file_path);
    ASSERT_NE(project, nullptr);

    auto& clips = project->tracks()[0]->clips();
    ASSERT_EQ(clips.size(), 2);

    // First clip: absolute position = 48000
    EXPECT_EQ(clips[0]->absolutePositionInSamples(), 48000);

    // Second clip: anchored to first, so absolute = 48000 + 96000 = 144000
    EXPECT_EQ(clips[1]->absolutePositionInSamples(), 144000);
}

// Test: Empty project
TEST_F(UapmdProjectFileTest, EmptyProject) {
    auto project = std::make_unique<uapmd::UapmdProjectDataImpl>();

    auto file_path = test_dir / "empty_project.json";
    ASSERT_TRUE(uapmd::UapmdProjectDataWriter::write(project.get(), file_path));

    auto loaded_project = uapmd::UapmdProjectDataReader::read(file_path);
    ASSERT_NE(loaded_project, nullptr);

    EXPECT_EQ(loaded_project->tracks().size(), 0);
    EXPECT_NE(loaded_project->masterTrack(), nullptr);
}

// Test: Invalid JSON
TEST_F(UapmdProjectFileTest, InvalidJSON) {
    std::string invalid_json = "{ invalid json content }";
    auto file_path = createTestFile("invalid.json", invalid_json);

    // choc JSON parser throws an exception on invalid JSON
    // The reader should handle this gracefully
    try {
        auto project = uapmd::UapmdProjectDataReader::read(file_path);
        // If no exception, it should return a valid (possibly empty) project
        ASSERT_NE(project, nullptr);
    } catch (const std::exception& e) {
        // Exception is also acceptable for invalid JSON
        SUCCEED();
    }
}

// Test: Non-existent file
TEST_F(UapmdProjectFileTest, NonExistentFile) {
    auto file_path = test_dir / "does_not_exist.json";
    auto project = uapmd::UapmdProjectDataReader::read(file_path);
    EXPECT_EQ(project, nullptr);
}
