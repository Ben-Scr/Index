using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Axiom;

//INFO(Ben-Scr): Used for adding vertical spacing before a field within the editor inspector ui
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public class SpaceAttribute : Attribute
{
    public float Height { get; }

    public SpaceAttribute(float height = 8.0f)
    {
        Height = height;
    }
}
