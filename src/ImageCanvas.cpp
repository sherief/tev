// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/FalseColor.h>
#include <tev/ImageCanvas.h>
#include <tev/ThreadPool.h>

#include <tev/imageio/ImageSaver.h>

#include <nanogui/theme.h>
#include <nanogui/screen.h>

#include <fstream>
#include <numeric>

using namespace Eigen;
using namespace filesystem;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

ImageCanvas::ImageCanvas(nanogui::Widget* parent, float pixelRatio)
: GLCanvas(parent), mPixelRatio(pixelRatio) {
    setDrawBorder(false);
}

bool ImageCanvas::scrollEvent(const Vector2i& p, const Vector2f& rel) {
    if (GLCanvas::scrollEvent(p, rel)) {
        return true;
    }

    float scaleAmount = rel.y();
    auto* glfwWindow = screen()->glfwWindow();
    // There is no explicit access to the currently pressed modifier keys here, so we
    // need to directly ask GLFW.
    if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
        scaleAmount /= 10;
    } else if (glfwGetKey(glfwWindow, SYSTEM_COMMAND_LEFT) || glfwGetKey(glfwWindow, SYSTEM_COMMAND_RIGHT)) {
        scaleAmount /= std::log2(1.1f);
    }

    scale(scaleAmount, p.cast<float>());
    return true;
}

void ImageCanvas::drawGL() {
    auto* glfwWindow = screen()->glfwWindow();
    Image* image = (mReference && glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT)) ? mReference.get() : mImage.get();

    if (!image) {
        mShader.draw(
            2.0f * mSize.cast<float>().cwiseInverse() / mPixelRatio,
            Vector2f::Constant(20)
        );
        return;
    }

    if (!mReference || glfwGetKey(glfwWindow, GLFW_KEY_LEFT_CONTROL) || image == mReference.get()) {
        mShader.draw(
            2.0f * mSize.cast<float>().cwiseInverse() / mPixelRatio,
            Vector2f::Constant(20),
            image->texture(getChannels(*image)),
            // The uber shader operates in [-1, 1] coordinates and requires the _inserve_
            // image transform to obtain texture coordinates in [0, 1]-space.
            transform(image).inverse().matrix(),
            mExposure,
            mOffset,
            mGamma,
            mTonemap
        );
        return;
    }

    mShader.draw(
        2.0f * mSize.cast<float>().cwiseInverse() / mPixelRatio,
        Vector2f::Constant(20),
        mImage->texture(getChannels(*mImage)),
        // The uber shader operates in [-1, 1] coordinates and requires the _inserve_
        // image transform to obtain texture coordinates in [0, 1]-space.
        transform(mImage.get()).inverse().matrix(),
        mReference->texture(getChannels(*mReference)),
        transform(mReference.get()).inverse().matrix(),
        mExposure,
        mOffset,
        mGamma,
        mTonemap,
        mMetric
    );
}

void ImageCanvas::draw(NVGcontext *ctx) {
    GLCanvas::draw(ctx);

    if (mImage) {
        auto texToNano = textureToNanogui(mImage.get());
        auto nanoToTex = texToNano.inverse();

        Vector2f pixelSize = texToNano * Vector2f::Ones() - texToNano * Vector2f::Zero();

        Vector2f topLeft = (nanoToTex * Vector2f::Zero());
        Vector2f bottomRight = (nanoToTex * mSize.cast<float>());

        Vector2i startIndices = Vector2i{
            static_cast<int>(floor(topLeft.x())),
            static_cast<int>(floor(topLeft.y())),
        };

        Vector2i endIndices = Vector2i{
            static_cast<int>(ceil(bottomRight.x())),
            static_cast<int>(ceil(bottomRight.y())),
        };

        if (pixelSize.x() > 50 && pixelSize.x() < 1024) {
            float fontSize = pixelSize.x() / 6;
            float fontAlpha = min(min(1.0f, (pixelSize.x() - 50) / 30), (1024 - pixelSize.x()) / 256);

            vector<string> channels = getChannels(*mImage);
            // Remove duplicates
            channels.erase(unique(begin(channels), end(channels)), end(channels));

            vector<Color> colors;
            for (const auto& channel : channels) {
                colors.emplace_back(Channel::color(channel));
            }

            nvgFontSize(ctx, fontSize);
            nvgFontFace(ctx, "sans");
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

            auto* glfwWindow = screen()->glfwWindow();
            bool altHeld = glfwGetKey(glfwWindow, GLFW_KEY_LEFT_ALT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_ALT);

            Vector2i cur;
            vector<float> values;
            for (cur.y() = startIndices.y(); cur.y() < endIndices.y(); ++cur.y()) {
                for (cur.x() = startIndices.x(); cur.x() < endIndices.x(); ++cur.x()) {
                    Vector2i nano = (texToNano * (cur.cast<float>() + Vector2f::Constant(0.5f))).cast<int>();
                    getValuesAtNanoPos(nano, values, channels);

                    TEV_ASSERT(values.size() >= colors.size(), "Can not have more values than channels.");

                    for (size_t i = 0; i < colors.size(); ++i) {
                        string str;
                        Vector2f pos;
                        
                        if (altHeld) {
                            float tonemappedValue = Channel::tail(channels[i]) == "A" ? values[i] : toSRGB(values[i]);
                            unsigned char discretizedValue = (char)(tonemappedValue * 255 + 0.5f);
                            str = tfm::format("%02X", discretizedValue);

                            pos = Vector2f{
                                mPos.x() + nano.x() + (i - 0.5f * (colors.size() - 1)) * fontSize * 0.88f,
                                mPos.y() + nano.y(),
                            };
                        } else {
                            str = tfm::format("%.4f", values[i]);

                            pos = Vector2f{
                                mPos.x() + nano.x(),
                                mPos.y() + nano.y() + (i - 0.5f * (colors.size() - 1)) * fontSize,
                            };
                        }

                        Color col = colors[i];
                        nvgFillColor(ctx, Color(col.r(), col.g(), col.b(), fontAlpha));
                        drawTextWithShadow(ctx, pos.x(), pos.y(), str, fontAlpha);
                    }
                }
            }
        }
    }

    // If we're not in fullscreen mode draw an inner drop shadow. (adapted from nanogui::Window)
    if (mPos.x() != 0) {
        int ds = mTheme->mWindowDropShadowSize, cr = mTheme->mWindowCornerRadius;
        NVGpaint shadowPaint = nvgBoxGradient(
            ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y(), cr * 2, ds * 2,
            mTheme->mTransparent, mTheme->mDropShadow
        );

        nvgSave(ctx);
        nvgResetScissor(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, mPos.x(), mPos.y(), mSize.x(), mSize.y());
        nvgRoundedRect(ctx, mPos.x() + ds, mPos.y() + ds, mSize.x() - 2 * ds, mSize.y() - 2 * ds, cr);
        nvgPathWinding(ctx, NVG_HOLE);
        nvgFillPaint(ctx, shadowPaint);
        nvgFill(ctx);
        nvgRestore(ctx);
    }
}

void ImageCanvas::translate(const Vector2f& amount) {
    mTransform = Translation2f(amount) * mTransform;
}

void ImageCanvas::scale(float amount, const Vector2f& origin) {
    float scaleFactor = pow(1.1f, amount);

    // Use the current cursor position as the origin to scale around.
    Vector2f offset = -(origin - position().cast<float>()) + 0.5f * mSize.cast<float>();
    auto scaleTransform =
        Translation2f(-offset) *
        Scaling(scaleFactor) *
        Translation2f(offset);

    mTransform = scaleTransform * mTransform;
}

float ImageCanvas::applyExposureAndOffset(float value) const {
    return pow(2.0f, mExposure) * value + mOffset;
}

vector<string> ImageCanvas::getChannels(const Image& image, const string& requestedLayer) {
    vector<vector<string>> groups = {
        { "R", "G", "B" },
        { "r", "g", "b" },
        { "X", "Y", "Z" },
        { "x", "y", "z" },
        { "U", "V" },
        { "u", "v" },
        { "Z" },
        { "z" },
    };

    string layerPrefix = requestedLayer.empty() ? "" : (requestedLayer + ".");

    vector<string> result;
    for (const auto& group : groups) {
        for (size_t i = 0; i < group.size(); ++i) {
            const auto& name = layerPrefix + group[i];
            if (image.hasChannel(name)) {
                result.emplace_back(name);
            }
        }

        if (!result.empty()) {
            break;
        }
    }

    string alphaChannelName = layerPrefix + "A";

    // No channels match the given groups; fall back to the first 3 channels.
    if (result.empty()) {
        const auto& channelNames = image.channelsInLayer(requestedLayer);
        for (const auto& name : channelNames) {
            if (name != alphaChannelName) {
                result.emplace_back(name);
            }

            if (result.size() >= 3) {
                break;
            }
        }
    }

    // If we found just 1 channel, let's display is as grayscale by duplicating it twice.
    if (result.size() == 1) {
        result.push_back(result[0]);
        result.push_back(result[0]);
    }

    // If there is an alpha layer, use it
    if (image.hasChannel(alphaChannelName)) {
        result.emplace_back(alphaChannelName);
    }

    // If we found just an alpha channel, let's display is as grayscale by duplicating it twice
    // (similar to the single-non-alpha-channel case).
    if (result.size() == 1) {
        result.push_back(result[0]);
        result.push_back(result[0]);
    }

    return result;
}

Vector2i ImageCanvas::getImageCoords(const Image& image, Vector2i mousePos) {
    Vector2f imagePos = textureToNanogui(&image).inverse() * mousePos.cast<float>();
    return {
        static_cast<int>(floor(imagePos.x())),
        static_cast<int>(floor(imagePos.y())),
    };
}

void ImageCanvas::getValuesAtNanoPos(Vector2i nanoPos, vector<float>& result, const vector<string>& channels) {
    result.clear();
    if (!mImage) {
        return;
    }

    Vector2i imageCoords = getImageCoords(*mImage, nanoPos);
    for (const auto& channel : channels) {
        result.push_back(mImage->channel(channel)->eval(imageCoords));
    }

    // Subtract reference if it exists.
    if (mReference) {
        Vector2i referenceCoords = getImageCoords(*mReference, nanoPos);
        const auto& referenceChannels = getChannels(*mReference);
        for (size_t i = 0; i < result.size(); ++i) {
            float reference = i < referenceChannels.size() ?
                mReference->channel(referenceChannels[i])->eval(referenceCoords) :
                0.0f;

            result[i] = applyMetric(result[i], reference);
        }
    }
}

Vector3f ImageCanvas::applyTonemap(const Vector3f& value, float gamma, ETonemap tonemap) {
    Vector3f result;
    switch (tonemap) {
        case ETonemap::SRGB:
            {
                result = {toSRGB(value.x()), toSRGB(value.y()), toSRGB(value.z())};
                break;
            }
        case ETonemap::Gamma:
            {
                result = {pow(value.x(), 1 / gamma), pow(value.y(), 1 / gamma), pow(value.z(), 1 / gamma)};
                break;
            }
        case ETonemap::FalseColor:
            {
                static const auto falseColor = [](float linear) {
                    static const auto& fcd = falseColorData();
                    int start = 4 * clamp((int)(linear * (fcd.size() / 4)), 0, (int)fcd.size() / 4 - 1);
                    return Vector3f{fcd[start], fcd[start + 1], fcd[start + 2]};
                };

                result = falseColor(log2(value.mean()) / 10 + 0.5f);
                break;
            }
        case ETonemap::PositiveNegative:
            {
                result = {-2.0f * value.cwiseMin(Vector3f::Zero()).mean(), 2.0f * value.cwiseMax(Vector3f::Zero()).mean(), 0.0f};
                break;
            }
        default:
            throw runtime_error{"Invalid tonemap selected."};
    }

    return result.cwiseMax(Vector3f::Zero()).cwiseMin(Vector3f::Ones());
}

float ImageCanvas::applyMetric(float image, float reference, EMetric metric) {
    float diff = image - reference;
    switch (metric) {
        case EMetric::Error:                 return diff;
        case EMetric::AbsoluteError:         return abs(diff);
        case EMetric::SquaredError:          return diff * diff;
        case EMetric::RelativeAbsoluteError: return abs(diff) / (reference + 0.01f);
        case EMetric::RelativeSquaredError:  return diff * diff / (reference * reference + 0.01f);
        default:
            throw runtime_error{"Invalid metric selected."};
    }
}

void ImageCanvas::fitImageToScreen(const Image& image) {
    Vector2f nanoguiImageSize = image.size().cast<float>() / mPixelRatio;
    mTransform = Scaling(mSize.cast<float>().cwiseQuotient(nanoguiImageSize).minCoeff());
}

void ImageCanvas::resetTransform() {
    mTransform = Affine2f::Identity();
}

std::vector<float> ImageCanvas::getHdrImageData(bool premultiplyAlpha) const {
    std::vector<float> result;

    if (!mImage) {
        return result;
    }

    const auto& channels = channelsFromImages(mImage, mReference, mRequestedLayer, mMetric);
    auto numPixels = mImage->count();

    if (channels.empty()) {
        return result;
    }

    int nChannelsToSave = std::min((int)channels.size(), 4);

    // Flatten image into vector
    result.resize(4 * numPixels, 0);

    ThreadPool pool;
    pool.parallelFor(0, nChannelsToSave, [&channels, &result](int i) {
        const auto& channelData = channels[i].data();
        for (DenseIndex j = 0; j < channelData.size(); ++j) {
            result[j * 4 + i] = channelData(j);
        }
    });

    // Manually set alpha channel to 1 if the image does not have one.
    if (nChannelsToSave < 4) {
        for (DenseIndex i = 0; i < numPixels; ++i) {
            result[i * 4 + 3] = 1;
        }
    }

    // Premultiply alpha if needed
    if (premultiplyAlpha) {
        pool.parallelFor(0, min(nChannelsToSave, 3), [&result,numPixels](int i) {
            for (DenseIndex j = 0; j < numPixels; ++j) {
                result[j * 4 + i] *= result[j * 4 + 3];
            }
        });
    }

    return result;
}

std::vector<char> ImageCanvas::getLdrImageData(bool premultiplyAlpha) const {
    std::vector<char> result;

    if (!mImage) {
        return result;
    }

    auto numPixels = mImage->count();
    auto floatData = getHdrImageData(premultiplyAlpha);

    // Store as LDR image.
    result.resize(floatData.size());

    ThreadPool pool;
    pool.parallelFor<DenseIndex>(0, numPixels, [&](DenseIndex i) {
        size_t start = 4 * i;
        Vector3f value = applyTonemap({
            applyExposureAndOffset(floatData[start]),
            applyExposureAndOffset(floatData[start + 1]),
            applyExposureAndOffset(floatData[start + 2]),
        });
        for (int j = 0; j < 3; ++j) {
            floatData[start + j] = value[j];
        }
        for (int j = 0; j < 4; ++j) {
            result[start + j] = (char)(floatData[start + j] * 255 + 0.5f);
        }
    });

    return result;
}

void ImageCanvas::saveImage(const path& path) const {
    if (!mImage) {
        return;
    }

    Vector2i imageSize = mImage->size();

    tlog::info() << "Saving currently displayed image as '" << path << "'.";
    auto start = chrono::system_clock::now();

    ofstream f{nativeString(path), ios_base::binary};
    if (!f) {
        throw invalid_argument{tfm::format("Could not open file %s", path)};
    }

    for (const auto& saver : ImageSaver::getSavers()) {
        if (!saver->canSaveFile(path)) {
            continue;
        }

        const auto* hdrSaver = dynamic_cast<const TypedImageSaver<float>*>(saver.get());
        const auto* ldrSaver = dynamic_cast<const TypedImageSaver<char>*>(saver.get());

        TEV_ASSERT(hdrSaver || ldrSaver, "Each image saver must either be a HDR or an LDR saver.");

        if (hdrSaver) {
            hdrSaver->save(f, path, getHdrImageData(saver->hasPremultipliedAlpha()), imageSize, 4);
        } else if (ldrSaver) {
            ldrSaver->save(f, path, getLdrImageData(saver->hasPremultipliedAlpha()), imageSize, 4);
        }

        auto end = chrono::system_clock::now();
        chrono::duration<double> elapsedSeconds = end - start;

        tlog::success() << tfm::format("Saved '%s' after %.3f seconds.", path, elapsedSeconds.count());
        return;
    }

    throw invalid_argument{tfm::format("No save routine for image type '%s' found.", path.extension())};
}

shared_ptr<Lazy<shared_ptr<CanvasStatistics>>> ImageCanvas::canvasStatistics() {
    if (!mImage) {
        return nullptr;
    }

    string channels = join(getChannels(*mImage), ",");
    string key = mReference ?
        tfm::format("%d-%s-%d-%d", mImage->id(), channels, mReference->id(), mMetric) :
        tfm::format("%d-%s", mImage->id(), channels);

    auto iter = mMeanValues.find(key);
    if (iter != end(mMeanValues)) {
        return iter->second;
    }

    auto image = mImage, reference = mReference;
    auto requestedLayer = mRequestedLayer;
    auto metric = mMetric;
    mMeanValues.insert(make_pair(key, make_shared<Lazy<shared_ptr<CanvasStatistics>>>([image, reference, requestedLayer, metric]() {
        return computeCanvasStatistics(image, reference, requestedLayer, metric);
    }, &mMeanValueThreadPool)));

    auto val = mMeanValues.at(key);
    val->computeAsync();
    return val;
}

vector<Channel> ImageCanvas::channelsFromImages(
    shared_ptr<Image> image,
    shared_ptr<Image> reference,
    const string& requestedLayer,
    EMetric metric
) {
    if (!image) {
        return {};
    }

    vector<Channel> result;
    const auto& channelNames = getChannels(*image, requestedLayer);
    for (size_t i = 0; i < channelNames.size(); ++i) {
        result.emplace_back(toUpper(Channel::tail(channelNames[i])), image->size());
    }

    bool onlyAlpha = all_of(begin(result), end(result), [](const Channel& c) { return c.name() == "A"; });

    if (!reference) {
        ThreadPool pool;
        pool.parallelFor(0, (int)channelNames.size(), [&](int i) {
            const auto* chan = image->channel(channelNames[i]);
            for (DenseIndex j = 0; j < chan->count(); ++j) {
                result[i].at(j) = chan->eval(j);
            }
        });
    } else {
        Vector2i size = image->size();
        Vector2i offset = (reference->size() - size) / 2;
        const auto& referenceChannels = getChannels(*reference, requestedLayer);

        ThreadPool pool;
        pool.parallelFor<size_t>(0, channelNames.size(), [&](size_t i) {
            const auto* chan = image->channel(channelNames[i]);
            bool isAlpha = !onlyAlpha && result[i].name() == "A";

            if (i < referenceChannels.size()) {
                const Channel* referenceChan = reference->channel(referenceChannels[i]);
                if (isAlpha) {
                    for (int y = 0; y < size.y(); ++y) {
                        for (int x = 0; x < size.x(); ++x) {
                            result[i].at({x, y}) = 0.5f * (
                                chan->eval({x, y}) +
                                referenceChan->eval({x + offset.x(), y + offset.y()})
                            );
                        }
                    }
                } else {
                    for (int y = 0; y < size.y(); ++y) {
                        for (int x = 0; x < size.x(); ++x) {
                            result[i].at({x, y}) = ImageCanvas::applyMetric(
                                chan->eval({x, y}),
                                referenceChan->eval({x + offset.x(), y + offset.y()}),
                                metric
                            );
                        }
                    }
                }
            } else {
                if (isAlpha) {
                    for (int y = 0; y < size.y(); ++y) {
                        for (int x = 0; x < size.x(); ++x) {
                            result[i].at({x, y}) = chan->eval({x, y});
                        }
                    }
                } else {
                    for (int y = 0; y < size.y(); ++y) {
                        for (int x = 0; x < size.x(); ++x) {
                            result[i].at({x, y}) = ImageCanvas::applyMetric(chan->eval({x, y}), 0, metric);
                        }
                    }
                }
            }
        });
    }

    return result;
}

shared_ptr<CanvasStatistics> ImageCanvas::computeCanvasStatistics(
    std::shared_ptr<Image> image,
    std::shared_ptr<Image> reference,
    const std::string& requestedLayer,
    EMetric metric
) {
    auto flattened = channelsFromImages(image, reference, requestedLayer, metric);

    float mean = 0;
    float maximum = -numeric_limits<float>::infinity();
    float minimum = numeric_limits<float>::infinity();

    const Channel* alphaChannel = nullptr;
    // Only treat the alpha channel specially if it is not the only channel of the image.
    if (!all_of(begin(flattened), end(flattened), [](const Channel& c) { return c.name() == "A"; })) {
        for (auto& channel : flattened) {
            if (channel.name() == "A") {
                alphaChannel = &channel;
                // The following code expects the alpha channel to be the last, so let's make sure it is.
                if (alphaChannel != &flattened.back()) {
                    swap(channel, flattened.back());
                }
                break;
            }
        }
    }

    int nChannels = alphaChannel ? (int)flattened.size() - 1 : (int)flattened.size();

    for (int i = 0; i < nChannels; ++i) {
        const auto& channel = flattened[i];
        mean += channel.data().mean();
        maximum = max(maximum, channel.data().maxCoeff());
        minimum = min(minimum, channel.data().minCoeff());
    }

    auto result = make_shared<CanvasStatistics>();

    result->mean = nChannels > 0 ? (mean / nChannels) : 0;
    result->maximum = maximum;
    result->minimum = minimum;

    // Now that we know the maximum and minimum value we can define our histogram bin size.
    static const int NUM_BINS = 400;
    result->histogram = MatrixXf::Zero(NUM_BINS, nChannels);

    // We're going to draw our histogram in log space.
    static const float addition = 0.001f;
    static const float smallest = log(addition);
    auto symmetricLog = [](float val) {
        return val > 0 ? (log(val + addition) - smallest) : -(log(-val + addition) - smallest);
    };
    auto symmetricLogInverse = [](float val) {
        return val > 0 ? (exp(val + smallest) - addition) : -(exp(-val + smallest) - addition);
    };

    float minLog = symmetricLog(minimum);
    float diffLog = symmetricLog(maximum) - minLog;

    auto valToBin = [&](float val) {
        return clamp((int)(NUM_BINS * (symmetricLog(val) - minLog) / diffLog), 0, NUM_BINS - 1);
    };

    result->histogramZero = valToBin(0);

    auto binToVal = [&](float val) {
        return symmetricLogInverse((diffLog * val / NUM_BINS) + minLog);
    };

    // In the strange case that we have 0 channels, early return, because the histogram makes no sense.
    if (nChannels == 0) {
        return result;
    }

    auto numElements = image->count();
    Eigen::MatrixXf indices(numElements, nChannels);

    ThreadPool pool;
    for (int i = 0; i < nChannels; ++i) {
        const auto& channel = flattened[i];
        pool.parallelForNoWait<DenseIndex>(0, numElements, [&, i](DenseIndex j) {
            indices(j, i) = valToBin(channel.eval(j));
        });
    }
    pool.waitUntilFinished();

    pool.parallelFor(0, nChannels, [&](int i) {
        for (DenseIndex j = 0; j < numElements; ++j) {
            result->histogram(indices(j, i), i) += alphaChannel ? alphaChannel->eval(j) : 1;
        }
    });

    for (int i = 0; i < NUM_BINS; ++i) {
        result->histogram.row(i) /= binToVal(i + 1) - binToVal(i);
    }

    // Normalize the histogram according to the 10th-largest
    // element to avoid a couple spikes ruining the entire graph.
    MatrixXf temp = result->histogram;
    DenseIndex idx = temp.size() - 10;
    nth_element(temp.data(), temp.data() + idx, temp.data() + temp.size());
    result->histogram /= max(temp(idx), 0.1f) * 1.3f;

    return result;
}

Vector2f ImageCanvas::pixelOffset(const Vector2i& size) const {
    // Translate by half of a pixel to avoid pixel boundaries aligning perfectly with texels.
    // The translation only needs to happen for axes with even resolution. Odd-resolution
    // axes are implicitly shifted by half a pixel due to the centering operation.
    // Additionally, add 0.1111111 such that our final position is almost never 0
    // modulo our pixel ratio, which again avoids aligned pixel boundaries with texels.
    return Vector2f{
        size.x() % 2 == 0 ?  0.5f : 0.0f,
        size.y() % 2 == 0 ? -0.5f : 0.0f,
    } + Vector2f::Constant(0.1111111f);
}

Transform<float, 2, 2> ImageCanvas::transform(const Image* image) {
    if (!image) {
        return Transform<float, 2, 0>::Identity();
    }

    // Center image, scale to pixel space, translate to desired position,
    // then rescale to the [-1, 1] square for drawing.
    return
        Scaling(2.0f / mSize.x(), -2.0f / mSize.y()) *
        mTransform *
        Scaling(1.0f / mPixelRatio) *
        Translation2f(pixelOffset(image->size())) *
        Scaling(image->size().cast<float>()) *
        Translation2f(Vector2f::Constant(-0.5f));
}

Transform<float, 2, 2> ImageCanvas::textureToNanogui(const Image* image) {
    if (!image) {
        return Transform<float, 2, 0>::Identity();
    }

    // Move origin to centre of image, scale pixels, apply our transform, move origin back to top-left.
    return
        Translation2f(0.5f * mSize.cast<float>()) *
        mTransform *
        Scaling(1.0f / mPixelRatio) *
        Translation2f(-0.5f * image->size().cast<float>() + pixelOffset(image->size()));
}

TEV_NAMESPACE_END
