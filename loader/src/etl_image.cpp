/*
 Copyright 2016 Nervana Systems Inc.
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

#include "etl_image.hpp"

using namespace std;
using namespace nervana;

image::config::config(nlohmann::json js)
{
    if(js.is_null()) {
        throw std::runtime_error("missing image config in json config");
    }

    for(auto& info : config_list) {
        info->parse(js);
    }
    verify_config("image", config_list, js);

    // Now fill in derived
    shape_t shape;
    if (flip_enable) {
        flip_distribution = bernoulli_distribution{0.5};
    }

    if (!center) {
        crop_offset = uniform_real_distribution<float> {0.0f, 1.0f};
    }

    if (channel_major) {
        shape = {channels, height, width};
    } else{
        shape = {height, width, channels};
    }
    add_shape_type(shape, type_string);

    validate();
}

void image::config::validate() {
    if(crop_offset.param().a() > crop_offset.param().b()) {
        throw std::invalid_argument("invalid crop_offset");
    }
    if(width <= 0) {
        throw std::invalid_argument("invalid width");
    }
    if(height <= 0) {
        throw std::invalid_argument("invalid height");
    }
}

void image::params::dump(ostream & ostr)
{
    ostr << "Angle: " << setw(3) << angle << " ";
    ostr << "Flip: " << flip << " ";
    ostr << "Lighting: ";
    for_each (lighting.begin(), lighting.end(), [&ostr](float &l) {ostr << l << " ";});
    ostr << "Photometric: ";
    for_each (photometric.begin(), photometric.end(), [&ostr](float &p) {ostr << p << " ";});
    ostr << "\n" << "Crop Box: " << cropbox << "\n";
}


/* Extract */
image::extractor::extractor(const image::config& cfg)
{
    if (!(cfg.channels == 1 || cfg.channels == 3))
    {
        std::stringstream ss;
        ss << "Unsupported number of channels in image: " << cfg.channels;
        throw std::runtime_error(ss.str());
    } else {
        _pixel_type = CV_MAKETYPE(CV_8U, cfg.channels);
        _color_mode = cfg.channels == 1 ? CV_LOAD_IMAGE_GRAYSCALE : CV_LOAD_IMAGE_COLOR;
    }
}

shared_ptr<image::decoded> image::extractor::extract(const char* inbuf, int insize)
{
    cv::Mat output_img;

    // It is bad to cast away const, but opencv does not support a const Mat
    // The Mat is only used for imdecode on the next line so it is OK here
    cv::Mat input_img(1, insize, _pixel_type, const_cast<char*>(inbuf));
    cv::imdecode(input_img, _color_mode, &output_img);

    auto rc = make_shared<image::decoded>();
    rc->add(output_img);    // don't need to check return for single image
    return rc;
}


/* Transform:
    image::config will be a supplied bunch of params used by this provider.
    on each record, the transformer will use the config along with the supplied
    record to fill a transform_params structure which will have

    Spatial distortion params:
    randomly sampled crop box (based on params->center, params->horizontal_distortion, params->scale_pct, record size)
    randomly determined flip (based on params->flip)
    randomly sampled rotation angle (based on params->angle)

    Photometric distortion params:
    randomly sampled contrast, brightness, saturation, lighting values (based on params->cbs, lighting bounds)

*/

image::transformer::transformer(const image::config&)
{
}

shared_ptr<image::decoded> image::transformer::transform(
                                                 shared_ptr<image::params> img_xform,
                                                 shared_ptr<image::decoded> img)
{
    vector<cv::Mat> finalImageList;
    for(int i=0; i<img->get_image_count(); i++) {
        cv::Mat rotatedImage;
        rotate(img->get_image(i), rotatedImage, img_xform->angle);

        cv::Mat croppedImage = rotatedImage(img_xform->cropbox);

        cv::Mat resizedImage;
        image::resize(croppedImage, resizedImage, img_xform->output_size);
        photo.cbsjitter(resizedImage, img_xform->photometric);
        photo.lighting(resizedImage, img_xform->lighting, img_xform->color_noise_std);

        cv::Mat *finalImage = &resizedImage;
        cv::Mat flippedImage;
        if (img_xform->flip) {
            cv::flip(resizedImage, flippedImage, 1);
            finalImage = &flippedImage;
        }
        finalImageList.push_back(*finalImage);
    }

    auto rc = make_shared<image::decoded>();
    if(rc->add(finalImageList) == false) {
        rc = nullptr;
    }
    return rc;
}

shared_ptr<image::params>
image::param_factory::make_params(shared_ptr<const decoded> input)
{
    // Must use this method for creating a shared_ptr rather than make_shared
    // since the params default ctor is private and factory is friend
    // make_shared is not friend :(
    auto imgstgs = shared_ptr<image::params>(new image::params());

    imgstgs->output_size = cv::Size2i(_cfg.width, _cfg.height);

    imgstgs->angle = _cfg.angle(_dre);
    imgstgs->flip  = _cfg.flip_distribution(_dre);

    cv::Size2f in_size = input->get_image_size();

    float scale = _cfg.scale(_dre);
    float horizontal_distortion = _cfg.horizontal_distortion(_dre);
    cv::Size2f out_shape(_cfg.width * horizontal_distortion, _cfg.height);

    cv::Size2f cropbox_size = cropbox_max_proportional(in_size, out_shape);
    if(_cfg.do_area_scale) {
        throw std::runtime_error("area scale not implemented");
    } else {
        cropbox_size = cropbox_linear_scale(cropbox_size, scale);
    }

    float c_off_x = _cfg.crop_offset(_dre);
    float c_off_y = _cfg.crop_offset(_dre);

    cv::Point2f cropbox_origin = cropbox_shift(in_size, cropbox_size, c_off_x, c_off_y);
    imgstgs->cropbox = cv::Rect(cropbox_origin, cropbox_size);

    if (_cfg.lighting.stddev() != 0) {
        for( int i=0; i<3; i++ ) {
            imgstgs->lighting.push_back(_cfg.lighting(_dre));
        }
        imgstgs->color_noise_std = _cfg.lighting.stddev();
    }
    if (_cfg.photometric.a()!=_cfg.photometric.b()) {
        for( int i=0; i<3; i++ ) {
            imgstgs->photometric.push_back(_cfg.photometric(_dre));
        }
    }
    return imgstgs;
}

void image::loader::load(const std::vector<void*>& outlist, shared_ptr<image::decoded> input)
{
    char* outbuf = (char*)outlist[0];
    // TODO: Generalize this to also handle multi_crop case
    auto img = input->get_image(0);
    auto cv_type = _cfg.get_shape_type().get_otype().cv_type;
    auto element_size = _cfg.get_shape_type().get_otype().size;
    int image_size = img.channels() * img.total() * element_size;

    for (int i=0; i < input->get_image_count(); i++) {
        auto outbuf_i = outbuf + (i * image_size);
        img = input->get_image(i);
        vector<cv::Mat> source;
        vector<cv::Mat> target;
        vector<int>     from_to;

        source.push_back(img);
        if (_cfg.channel_major) {
            for(int ch=0; ch<_cfg.channels; ch++) {
                target.emplace_back(img.size(), cv_type, (char*)(outbuf_i + ch * img.total() * element_size));
                from_to.push_back(ch);
                from_to.push_back(ch);
            }
        } else {
            target.emplace_back(img.size(), CV_MAKETYPE(cv_type, _cfg.channels), (char*)(outbuf_i));
            for(int ch=0; ch<_cfg.channels; ch++) {
                from_to.push_back(ch);
                from_to.push_back(ch);
            }
        }
        image::convert_mix_channels(source, target, from_to);
    }
}

void image::loader::split(cv::Mat& img, char* buf)
{
    // split `img` into individual channels so that buf is in c
    int pix_per_channel = img.total();
    int num_channels = img.channels();

    if (num_channels == 1) {
        memcpy(buf, img.data, pix_per_channel);
    } else {
        // Split into separate channels
        cv::Size2i size = img.size();
        cv::Mat b(size, CV_8U, buf);
        cv::Mat g(size, CV_8U, buf + pix_per_channel);
        cv::Mat r(size, CV_8U, buf + 2 * pix_per_channel);

        cv::Mat channels[3] = {b, g, r};
        cv::split(img, channels);
    }
}

std::string dump_default(const std::string& s) { return s; }
std::string dump_default(int v) { return std::to_string(v); }
std::string dump_default(uint32_t v) { return std::to_string(v); }
std::string dump_default(size_t v) { return std::to_string(v); }
std::string dump_default(float v) { return std::to_string(v); }
std::string dump_default(const std::vector<float>& v)
{
    return "blah";
}

std::string dump_default(const std::vector<std::string>& v)
{
    return "blah";
}

std::string dump_default(const std::uniform_real_distribution<float>& v)
{
    return "blah";
}

std::string dump_default(const std::uniform_int_distribution<int>& v)
{
    return "blah";
}

std::string dump_default(const std::normal_distribution<float>& v)
{
    return "blah";
}

std::string dump_default(const std::bernoulli_distribution& v)
{
    return "blah";
}
