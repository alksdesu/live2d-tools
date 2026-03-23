#ifndef CUBISMJSONHOLDER_H
#define CUBISMJSONHOLDER_H


#include "framework/Utils/CubismJson.hpp"

//--------- LIVE2D NAMESPACE ------------
namespace Live2D { namespace Cubism { namespace Framework {
    /**
     * An interface that implements the CubismJson instantiation<br>
     * and validity checks at each JSON parser.
     */
    class CubismJsonHolder
    {
    public:
        /**
         * Constructor
         */
        CubismJsonHolder()
            : _json(NULL)
        { }

        /**
         * Destructor
         */
        virtual ~CubismJsonHolder()
        { }

        /**
         * Returns whether the CubismJson is valid or not.
         *
         * @return true if valid; otherwise false
         */
        csmBool IsValid()
        {
            return _json;
        }

    protected:

        /**
         * Make the instance of CubismJson.
         *
         * @param buffer Buffer into which JSON is loaded
         * @param size Number of bytes in buffer
         */
        void CreateCubismJson(const csmByte* buffer, csmSizeInt size)
        {
            _json = Utils::CubismJson::Create(buffer, size);

            if (!IsValid())
            {
                CubismLogError("[CubismJsonHolder] Invalid Json document.");
            }
        };

        /**
         * Destroy the instance of CubismJson.
         */
        void DeleteCubismJson()
        {
            Utils::CubismJson::Delete(_json);
            _json = NULL;
        }

        /** Instance of CubismJson */
        Utils::CubismJson* _json;
    };
}}}

#endif // CUBISMJSONHOLDER_H
