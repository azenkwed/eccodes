/*
 * (C) Copyright 2005- ECMWF.
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 *
 * In applying this licence, ECMWF does not waive the privileges and immunities granted to it by
 * virtue of its status as an intergovernmental organisation nor does it submit to any jurisdiction.
 */

#include "grib_api_internal.h"
#include <math.h>

/*
   This is used by make_class.pl

   START_CLASS_DEF
   CLASS      = iterator
   SUPER      = grib_iterator_class_gen
   IMPLEMENTS = destroy
   IMPLEMENTS = init;next
   MEMBERS    = double *lats
   MEMBERS    = double *lons
   MEMBERS    = long Nj
   END_CLASS_DEF
*/

/* START_CLASS_IMP */

/*

Don't edit anything between START_CLASS_IMP and END_CLASS_IMP
Instead edit values between START_CLASS_DEF and END_CLASS_DEF
or edit "iterator.class" and rerun ./make_class.pl

*/


static void init_class(grib_iterator_class*);

static int init(grib_iterator* i, grib_handle*, grib_arguments*);
static int next(grib_iterator* i, double* lat, double* lon, double* val);
static int destroy(grib_iterator* i);


typedef struct grib_iterator_mercator
{
    grib_iterator it;
    /* Members defined in gen */
    long carg;
    const char* missingValue;
    /* Members defined in mercator */
    double* lats;
    double* lons;
    long Nj;
} grib_iterator_mercator;

extern grib_iterator_class* grib_iterator_class_gen;

static grib_iterator_class _grib_iterator_class_mercator = {
    &grib_iterator_class_gen,                /* super                     */
    "mercator",                     /* name                      */
    sizeof(grib_iterator_mercator), /* size of instance          */
    0,                                       /* inited */
    &init_class,                             /* init_class */
    &init,                                   /* constructor               */
    &destroy,                                /* destructor                */
    &next,                                   /* Next Value                */
    0,                                       /*  Previous Value           */
    0,                                       /* Reset the counter         */
    0,                                       /* has next values           */
};

grib_iterator_class* grib_iterator_class_mercator = &_grib_iterator_class_mercator;


static void init_class(grib_iterator_class* c)
{
    c->previous = (*(c->super))->previous;
    c->reset    = (*(c->super))->reset;
    c->has_next = (*(c->super))->has_next;
}
/* END_CLASS_IMP */

#define EPSILON 1.0e-10

#ifndef M_PI
#define M_PI 3.14159265358979323846 /* Whole pie */
#endif

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923 /* Half a pie */
#endif

#ifndef M_PI_4
#define M_PI_4 0.78539816339744830962 /* Quarter of a pie */
#endif

#define RAD2DEG 57.29577951308232087684 /* 180 over pi */
#define DEG2RAD 0.01745329251994329576  /* pi over 180 */

/* Adjust longitude (in radians) to range -180 to 180 */
static double adjust_lon_radians(double lon)
{
    if (lon > M_PI)  lon -= 2 * M_PI;
    if (lon < -M_PI) lon += 2 * M_PI;
    return lon;
}

/* Function to compute the latitude angle, phi2, for the inverse
 * From the book "Map Projections-A Working Manual-John P. Snyder (1987)"
 * Equation (7–9) involves rapidly converging iteration: Calculate t from (15-11)
 * Then, assuming an initial trial phi equal to (pi/2 - 2*arctan t) in the right side of equation (7–9),
 * calculate phi on the left side. Substitute the calculated phi) into the right side,
 * calculate a new phi, etc., until phi does not change significantly from the preceding trial value of phi
 */
static double compute_phi(
    double eccent, /* Spheroid eccentricity */
    double ts,     /* Constant value t */
    int* error)
{
    double eccnth, phi, con, dphi, sinpi;
    int i, MAX_ITER = 15;

    eccnth = 0.5 * eccent;
    phi    = M_PI_2 - 2 * atan(ts);
    for (i = 0; i <= MAX_ITER; i++) {
        sinpi = sin(phi);
        con   = eccent * sinpi;
        dphi  = M_PI_2 - 2 * atan(ts * (pow(((1.0 - con) / (1.0 + con)), eccnth))) - phi;
        phi += dphi;
        if (fabs(dphi) <= 0.0000000001)
            return (phi);
    }
    *error = GRIB_INTERNAL_ERROR;
    return 0;
}

/* Compute the constant small m which is the radius of
   a parallel of latitude, phi, divided by the semimajor axis */
static double compute_m(double eccent, double sinphi, double cosphi)
{
    const double con = eccent * sinphi;
    return ((cosphi / (sqrt(1.0 - con * con))));
}

/* Compute the constant small t for use in the forward computations */
static double compute_t(
    double eccent, /* Eccentricity of the spheroid */
    double phi,    /* Latitude phi */
    double sinphi) /* Sine of the latitude */
{
    double con = eccent * sinphi;
    double com = 0.5 * eccent;
    con        = pow(((1.0 - con) / (1.0 + con)), com);
    return (tan(0.5 * (M_PI_2 - phi)) / con);
}

static double calculate_eccentricity(double minor, double major)
{
    const double temp = minor / major;
    return sqrt(1.0 - temp * temp);
}

static int init_sphere(grib_handle* h,
                       grib_iterator_mercator* self,
                       size_t nv, long nx, long ny,
                       double LoVInDegrees,
                       double Dx, double Dy, double radius,
                       double latFirstInRadians, double lonFirstInRadians,
                       double LoVInRadians, double Latin1InRadians, double Latin2InRadians,
                       double LaDInRadians,
                       long iScansNegatively, long jScansPositively, long jPointsAreConsecutive)
{
    int i, j;
    double f, n, rho, rho0, angle, x0, y0, x, y, tmp, tmp2;
    double latDeg, lonDeg, lonDiff;

    if (fabs(Latin1InRadians - Latin2InRadians) < 1E-09) {
        n = sin(Latin1InRadians);
    } else {
        n = log(cos(Latin1InRadians) / cos(Latin2InRadians)) /
            log(tan(M_PI_4 + Latin2InRadians / 2.0) / tan(M_PI_4 + Latin1InRadians / 2.0));
    }

    f    = (cos(Latin1InRadians) * pow(tan(M_PI_4 + Latin1InRadians / 2.0), n)) / n;
    rho  = radius * f * pow(tan(M_PI_4 + latFirstInRadians / 2.0), -n);
    rho0 = radius * f * pow(tan(M_PI_4 + LaDInRadians / 2.0), -n);
    if (n < 0) /* adjustment for southern hemisphere */
        rho0 = -rho0;
    lonDiff = lonFirstInRadians - LoVInRadians;

    /* Adjust longitude to range -180 to 180 */
    if (lonDiff > M_PI)
        lonDiff -= 2 * M_PI;
    if (lonDiff < -M_PI)
        lonDiff += 2 * M_PI;
    angle = n * lonDiff;
    x0    = rho * sin(angle);
    y0    = rho0 - rho * cos(angle);
    /*Dx = iScansNegatively == 0 ? Dx : -Dx;*/
    /* GRIB-405: Don't change sign of Dy. Latitudes ALWAYS increase from latitudeOfFirstGridPoint */
    /*Dy = jScansPositively == 1 ? Dy : -Dy;*/

    /* Allocate latitude and longitude arrays */
    self->lats = (double*)grib_context_malloc(h->context, nv * sizeof(double));
    if (!self->lats) {
        grib_context_log(h->context, GRIB_LOG_ERROR, "Unable to allocate %ld bytes", nv * sizeof(double));
        return GRIB_OUT_OF_MEMORY;
    }
    self->lons = (double*)grib_context_malloc(h->context, nv * sizeof(double));
    if (!self->lats) {
        grib_context_log(h->context, GRIB_LOG_ERROR, "Unable to allocate %ld bytes", nv * sizeof(double));
        return GRIB_OUT_OF_MEMORY;
    }

    /* Populate our arrays */
    for (j = 0; j < ny; j++) {
        y = y0 + j * Dy;
        if (n < 0) { /* adjustment for southern hemisphere */
            y = -y;
        }
        tmp  = rho0 - y;
        tmp2 = tmp * tmp;
        for (i = 0; i < nx; i++) {
            int index = i + j * nx;
            x         = x0 + i * Dx;
            if (n < 0) { /* adjustment for southern hemisphere */
                x = -x;
            }
            angle = atan2(x, tmp); /* See ECC-524 */
            rho   = sqrt(x * x + tmp2);
            if (n <= 0) rho = -rho;
            lonDeg = LoVInDegrees + (angle / n) * RAD2DEG;
            latDeg = (2.0 * atan(pow(radius * f / rho, 1.0 / n)) - M_PI_2) * RAD2DEG;
            lonDeg = normalise_longitude_in_degrees(lonDeg);
            self->lons[index] = lonDeg;
            self->lats[index] = latDeg;
        }
    }

    return GRIB_SUCCESS;
}

/* Oblate spheroid */
static int init_oblate(grib_handle* h,
                       grib_iterator_mercator* self,
                       size_t nv, long nx, long ny,
                       double DiInMetres, double DjInMetres,
                       double earthMinorAxisInMetres, double earthMajorAxisInMetres,
                       double latFirstInRadians, double lonFirstInRadians,
                       double latLastInRadians, double lonLastInRadians,
                       double LaDInRadians, double orientationInRadians)
{
    int i, j, err = 0;
    double x0, y0, x, y, latRad, lonRad, latDeg, lonDeg, sinphi, ts, rh1, theta;
    double false_easting;  /* x offset in meters */
    double false_northing; /* y offset in meters */
    double m1; /* small value m */

    double ns;     /* ratio of angle between meridian */
    double F;      /* flattening of ellipsoid */
    double rh;     /* height above ellipsoid  */
    double sin_po; /* sin value */
    double cos_po; /* cos value */
    double con;    /* temporary variable */
    double ms1;    /* small m 1 */
    double ms2;    /* small m 2 */
    double ts0;    /* small t 0 */
    double ts1;    /* small t 1 */
    double ts2;    /* small t 2 */
    double center_lat = LaDInRadians;
    double temp, e, es;

    //double e = calculate_eccentricity(earthMinorAxisInMetres, earthMajorAxisInMetres);
    temp = earthMinorAxisInMetres / earthMajorAxisInMetres;
    es = 1.0 - (temp*temp);
    e = sqrt(es);
    m1 = cos(center_lat)/(sqrt(1.0 - es * sin(center_lat) * sin(center_lat)));

//     sin_po = sin(Latin1InRadians);
//     cos_po = cos(Latin1InRadians);
//     con    = sin_po;
//     ms1    = compute_m(e, sin_po, cos_po);
//     ts1    = compute_t(e, Latin1InRadians, sin_po);
// 
//     sin_po = sin(Latin2InRadians);
//     cos_po = cos(Latin2InRadians);
//     ms2    = compute_m(e, sin_po, cos_po);
//     ts2    = compute_t(e, Latin2InRadians, sin_po);
//     sin_po = sin(LaDInRadians);
//     ts0    = compute_t(e, LaDInRadians, sin_po);
// 
//     if (fabs(Latin1InRadians - Latin2InRadians) > EPSILON) {
//         ns = log(ms1 / ms2) / log(ts1 / ts2);
//     } else {
//         ns = con;
//     }
//     F = ms1 / (ns * pow(ts1, ns));
//     rh = earthMajorAxisInMetres * F * pow(ts0, ns);
// 
//     /* Forward projection: convert lat,lon to x,y */
    if (fabs(fabs(latFirstInRadians) - M_PI_2)  <= EPSILON) {
        grib_context_log(h->context, GRIB_LOG_ERROR, "Mercator: Transformation cannot be computed at the poles");
        return GRIB_GEOCALCULUS_PROBLEM;
    } else {
        sinphi = sin(latFirstInRadians);
        ts = compute_t(e,latFirstInRadians,sinphi);
        x0 = earthMajorAxisInMetres * m1 * adjust_lon_radians(lonFirstInRadians - orientationInRadians);
        y0 = 0 - earthMajorAxisInMetres * m1 * log(ts);
    }
//     con = fabs(fabs(latFirstInRadians) - M_PI_2);
//     if (con > EPSILON) {
//         sinphi = sin(latFirstInRadians);
//         ts     = compute_t(e, latFirstInRadians, sinphi);
//         rh1    = earthMajorAxisInMetres * F * pow(ts, ns);
//     } else {
//         con = latFirstInRadians * ns;
//         if (con <= 0) {
//             grib_context_log(h->context, GRIB_LOG_ERROR, "Point cannot be projected");
//             return GRIB_GEOCALCULUS_PROBLEM;
//         }
//         rh1 = 0;
//     }
//     theta = ns * adjust_lon_radians(lonFirstInRadians - LoVInRadians);
//     x0    = rh1 * sin(theta);
//     y0    = rh - rh1 * cos(theta);
     x0    = -x0;
     y0    = -y0;

    /* Allocate latitude and longitude arrays */
    self->lats = (double*)grib_context_malloc(h->context, nv * sizeof(double));
    if (!self->lats) {
        grib_context_log(h->context, GRIB_LOG_ERROR, "Unable to allocate %ld bytes", nv * sizeof(double));
        return GRIB_OUT_OF_MEMORY;
    }
    self->lons = (double*)grib_context_malloc(h->context, nv * sizeof(double));
    if (!self->lats) {
        grib_context_log(h->context, GRIB_LOG_ERROR, "Unable to allocate %ld bytes", nv * sizeof(double));
        return GRIB_OUT_OF_MEMORY;
    }

    /* Populate our arrays */
    false_easting  = x0;
    false_northing = y0;
    for (j = 0; j < ny; j++) {
        y = j * DiInMetres;
        for (i = 0; i < nx; i++) {
            const int index = i + j * nx;
            double _x, _y;
            x = i * DiInMetres;
            /* Inverse projection to convert from x,y to lat,lon */
            _x = x - false_easting;
            _y = rh - y + false_northing;
            rh1 = sqrt(_x * _x + _y * _y);
            con = 1.0;
            if (ns <= 0) {
                rh1 = -rh1;
                con = -con;
            }
            theta = 0.0;
            if (rh1 != 0)
                theta = atan2((con * _x), (con * _y));
            if ((rh1 != 0) || (ns > 0.0)) {
                con    = 1.0 / ns;
                ts     = pow((rh1 / (earthMajorAxisInMetres * F)), con);
                latRad = compute_phi(e, ts, &err);
                if (err) {
                    grib_context_log(h->context, GRIB_LOG_ERROR, "Failed to compute the latitude angle, phi2, for the inverse");
                    grib_context_free(h->context, self->lats);
                    grib_context_free(h->context, self->lons);
                    return err;
                }
            } else {
                latRad = -M_PI_2;
            }
            lonRad = 0; // TODO adjust_lon_radians(theta / ns + LoVInRadians);
            latDeg = latRad * RAD2DEG;  /* Convert to degrees */
            lonDeg = lonRad * RAD2DEG;
            lonDeg = normalise_longitude_in_degrees(lonDeg);
            self->lons[index] = lonDeg;
            self->lats[index] = latDeg;
        }
    }
    return GRIB_SUCCESS;
}

static int init(grib_iterator* iter, grib_handle* h, grib_arguments* args)
{
    int err = 0, is_oblate = 0;
    long ni, nj, iScansNegatively, jScansPositively, jPointsAreConsecutive, alternativeRowScanning;
    double latFirstInDegrees, lonFirstInDegrees, LaDInDegrees;
    double latLastInDegrees, lonLastInDegrees, orientationInDegrees, DiInMetres, DjInMetres, radius = 0;
    double latFirstInRadians, lonFirstInRadians, latLastInRadians, lonLastInRadians,
        LaDInRadians, orientationInRadians;
    double earthMajorAxisInMetres=0, earthMinorAxisInMetres=0;

    grib_iterator_mercator* self = (grib_iterator_mercator*)iter;

    const char* sRadius            = grib_arguments_get_name(h, args, self->carg++);
    const char* sNi                = grib_arguments_get_name(h, args, self->carg++);
    const char* sNj                = grib_arguments_get_name(h, args, self->carg++);
    const char* sLatFirstInDegrees = grib_arguments_get_name(h, args, self->carg++);
    const char* sLonFirstInDegrees = grib_arguments_get_name(h, args, self->carg++);
    const char* sLaDInDegrees      = grib_arguments_get_name(h, args, self->carg++);
    const char* sLatLastInDegrees  = grib_arguments_get_name(h, args, self->carg++);
    const char* sLonLastInDegrees  = grib_arguments_get_name(h, args, self->carg++);
    const char* sOrientationInDegrees  = grib_arguments_get_name(h, args, self->carg++);
    /* Dx and Dy are in Metres */
    const char* sDi                     = grib_arguments_get_name(h, args, self->carg++);
    const char* sDj                     = grib_arguments_get_name(h, args, self->carg++);
    const char* siScansNegatively       = grib_arguments_get_name(h, args, self->carg++);
    const char* sjScansPositively       = grib_arguments_get_name(h, args, self->carg++);
    const char* sjPointsAreConsecutive  = grib_arguments_get_name(h, args, self->carg++);
    const char* sAlternativeRowScanning = grib_arguments_get_name(h, args, self->carg++);

    if ((err = grib_get_long_internal(h, sNi, &ni)) != GRIB_SUCCESS) return err;
    if ((err = grib_get_long_internal(h, sNj, &nj)) != GRIB_SUCCESS) return err;

    is_oblate = grib_is_earth_oblate(h);

    if (is_oblate) {
        if ((err = grib_get_double_internal(h, "earthMinorAxisInMetres", &earthMinorAxisInMetres)) != GRIB_SUCCESS) return err;
        if ((err = grib_get_double_internal(h, "earthMajorAxisInMetres", &earthMajorAxisInMetres)) != GRIB_SUCCESS) return err;
    } else {
        if ((err = grib_get_double_internal(h, sRadius, &radius)) != GRIB_SUCCESS) return err;
    }

    if (iter->nv != ni * nj) {
        grib_context_log(h->context, GRIB_LOG_ERROR, "Wrong number of points (%ld!=%ldx%ld)", iter->nv, ni, nj);
        return GRIB_WRONG_GRID;
    }

    if ((err = grib_get_double_internal(h, sLaDInDegrees, &LaDInDegrees)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_double_internal(h, sLatFirstInDegrees, &latFirstInDegrees)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_double_internal(h, sLonFirstInDegrees, &lonFirstInDegrees)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_double_internal(h, sLatLastInDegrees, &latLastInDegrees)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_double_internal(h, sLonLastInDegrees, &lonLastInDegrees)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_double_internal(h, sOrientationInDegrees, &orientationInDegrees)) != GRIB_SUCCESS)
        return err;

    if ((err = grib_get_double_internal(h, sDi, &DiInMetres)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_double_internal(h, sDj, &DjInMetres)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_long_internal(h, sjPointsAreConsecutive, &jPointsAreConsecutive)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_long_internal(h, sjScansPositively, &jScansPositively)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_long_internal(h, siScansNegatively, &iScansNegatively)) != GRIB_SUCCESS)
        return err;
    if ((err = grib_get_long_internal(h, sAlternativeRowScanning, &alternativeRowScanning)) != GRIB_SUCCESS)
        return err;

    latFirstInRadians    = latFirstInDegrees * DEG2RAD;
    lonFirstInRadians    = lonFirstInDegrees * DEG2RAD;
    latLastInRadians     = latLastInDegrees * DEG2RAD;
    lonLastInRadians     = lonLastInDegrees * DEG2RAD;
    LaDInRadians         = LaDInDegrees * DEG2RAD;
    orientationInRadians = orientationInDegrees * DEG2RAD;

//     if (is_oblate) {
//         err = init_oblate(h, self, iter->nv, ni, nj,
//                           DiInMetres, DjInMetres, earthMinorAxisInMetres, earthMajorAxisInMetres,
//                           latFirstInRadians, lonFirstInRadians,
//                           latLastInRadians, lonLastInRadians,
//                           LaDInRadians, orientationInRadians);
//     } else {
//         err = init_sphere(h, self, iter->nv, nx, ny,
//                           Dx, Dy, radius,
//                           latFirstInRadians, lonFirstInRadians,
//                           LoVInRadians, Latin1InRadians, Latin2InRadians, LaDInRadians,
//                           iScansNegatively, jScansPositively, jPointsAreConsecutive);
//     }
    if (err) return err;

    iter->e = -1;

    /* Apply the scanning mode flags which may require data array to be transformed */
    err = transform_iterator_data(h, iter->data,
                                  iScansNegatively, jScansPositively, jPointsAreConsecutive, alternativeRowScanning,
                                  iter->nv, ni, nj);
    return err;
}

static int next(grib_iterator* i, double* lat, double* lon, double* val)
{
    grib_iterator_mercator* self = (grib_iterator_mercator*)i;

    if ((long)i->e >= (long)(i->nv - 1))
        return 0;
    i->e++;

    *lat = self->lats[i->e];
    *lon = self->lons[i->e];
    *val = i->data[i->e];

    return 1;
}

static int destroy(grib_iterator* i)
{
    grib_iterator_mercator* self = (grib_iterator_mercator*)i;
    const grib_context* c                 = i->h->context;

    grib_context_free(c, self->lats);
    grib_context_free(c, self->lons);
    return 1;
}
